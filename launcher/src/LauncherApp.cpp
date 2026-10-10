#include "LauncherApp.hpp"
#include "Theme.hpp"
#include "Win32Util.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <windowsx.h>
#include <shlobj.h>
#include <cstdio>
#include <cmath>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace launcher {
namespace {
LauncherApp* g_app = nullptr;
constexpr UINT kFpsTimerId = 0; // unused; timers replaced by frame polling
const char* kTabNames[7] = {"Главная", "Библиотека", "Изображение", "Геймпад", "Диагностика", "Производительность", "Настройки"};
const char* kCoverNames[] = {"cover.jpg", "cover.png", "icon0.png"};
const char* kBackdropNames[] = {"pic1.png", "pic0.png", "cover.jpg", "icon0.png"};

std::string ExeName(const GameEntry& g) { return g.path + "\\app.exe"; }
bool FileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}
} // namespace

LauncherApp::LauncherApp() : lib_(cfg_), backend_([this](const std::string& s) {
    AppendLog(LauncherConfig::LogPath(), s);
}) {
    g_app = this;
}
LauncherApp::~LauncherApp() { g_app = nullptr; }

// ---------------------------------------------------------------- main window
LRESULT CALLBACK LauncherApp::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(l);
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    LauncherApp* self = reinterpret_cast<LauncherApp*>(GetWindowLongPtrA(h, GWLP_USERDATA));
    if (!self) return DefWindowProcA(h, m, w, l);
    // Feed ImGui input with the main context current.
    if (self->mainCtx_) {
        ImGui::SetCurrentContext(self->mainCtx_);
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    }
    return self->OnMain(h, m, w, l);
}

LRESULT LauncherApp::OnMain(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: {
            // Custom chrome: resizable edges + draggable title strip.
            if (IsMaximized(h)) return HTCLIENT;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            RECT rc{};
            GetWindowRect(h, &rc);
            const int b = 8;
            const bool L = pt.x < rc.left + b, R = pt.x >= rc.right - b;
            const bool T = pt.y < rc.top + b, B = pt.y >= rc.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
            // Title strip (drawn by ImGui, ~44*dpi tall), except caption buttons.
            if (pt.y < rc.top + (int)(44 * dpi_) && pt.x < rc.right - (int)(160 * dpi_)) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DPICHANGED: {
            RECT* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            dpi_ = DpiScaleForWindow(h);
            if (mainCtx_) RebuildFonts(mainCtx_, dpi_);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mi = reinterpret_cast<MINMAXINFO*>(l);
            mi->ptMinTrackSize.x = 1024; mi->ptMinTrackSize.y = 640;
            // Keep a maximized borderless window inside the work area.
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoA(mon, &info)) {
                mi->ptMaxPosition.x = info.rcWork.left;
                mi->ptMaxPosition.y = info.rcWork.top;
                mi->ptMaxSize.x = info.rcWork.right - info.rcWork.left;
                mi->ptMaxSize.y = info.rcWork.bottom - info.rcWork.top;
            }
            return 0;
        }
        case WM_DESTROY:
            quit_ = true;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

bool LauncherApp::CreateMain(HINSTANCE h) {
    hinst_ = h;
    WNDCLASSA wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = h;
    wc.lpszClassName = "AnyPS5LauncherUI";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    // Borderless custom chrome (ImGui-drawn titlebar): WS_POPUP + THICKFRAME
    // keeps Aero shadow, taskbar presence and Win+arrow snapping.
    mainHwnd_ = CreateWindowExA(WS_EX_APPWINDOW, "AnyPS5LauncherUI", "AnyPS5",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, h, this);
    if (!mainHwnd_) return false;
    // Dark dialogs (folder picker) on Windows 10+.
    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (dwm) {
        using Sig = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<Sig>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (fn) {
            BOOL dark = TRUE;
            fn(mainHwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark)); // Win10 18985+
            fn(mainHwnd_, 19 /*pre-18985*/, &dark, sizeof(dark));
        }
    }
    SetWindowTitleUTF8(mainHwnd_, "AnyPS5 — Windows Gaming Launcher");
    dpi_ = DpiScaleForWindow(mainHwnd_);
    // NOTE: the F1 hotkey is registered by the overlay thread (system-wide),
    // so it fires even while the game window is focused.
    return true;
}

// ---------------------------------------------------------------- overlay
// The overlay thread entry lives in Screens.cpp (OverlayThreadProc):
// own window, D3D11 device and ImGui context. Main thread only signals it.

// ---------------------------------------------------------------- app
GameEntry* LauncherApp::CurrentGame() {
    GameEntry* g = cfg_.FindGame(cfg_.selectedGame);
    if (!g && !cfg_.games.empty()) g = &cfg_.games[0];
    return g;
}

void LauncherApp::RebuildFonts(ImGuiContext* ctx, float dpi) {
    ImGui::SetCurrentContext(ctx);
    theme::LoadFonts(dpi);
    theme::ApplyGraphiteStyle(dpi);
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.0f;
}

void LauncherApp::FeedGamepad(ImGuiContext* ctx) {
    // Dear ImGui 1.91 gamepad API: digital via AddKeyEvent, analog sticks and
    // triggers via AddKeyAnalogEvent (replaces removed io.NavInputs[]).
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    const PadState& pad = input_.Primary();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, pad.a);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, pad.b);
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, pad.x);
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, pad.y);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, pad.dleft);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, pad.dright);
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, pad.dup);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, pad.ddown);
    io.AddKeyEvent(ImGuiKey_GamepadL1, pad.lb);
    io.AddKeyEvent(ImGuiKey_GamepadR1, pad.rb);
    io.AddKeyEvent(ImGuiKey_GamepadStart, pad.menu);
    io.AddKeyEvent(ImGuiKey_GamepadBack, pad.view);
    io.AddKeyEvent(ImGuiKey_GamepadL3, pad.l3);
    io.AddKeyEvent(ImGuiKey_GamepadR3, pad.r3);
    auto analog = [&](ImGuiKey neg, ImGuiKey pos, int v) {
        float f = v / 32768.0f;
        if (f > -0.25f && f < 0.25f) f = 0.0f;
        io.AddKeyAnalogEvent(neg, f < 0.0f, f < 0.0f ? -f : 0.0f);
        io.AddKeyAnalogEvent(pos, f > 0.0f, f > 0.0f ? f : 0.0f);
    };
    analog(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, pad.lx);
    analog(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, pad.ly);
    analog(ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight, pad.rx);
    analog(ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown, pad.ry);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, pad.lt > 4, pad.lt / 255.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, pad.rt > 4, pad.rt / 255.0f);
}

void LauncherApp::PollFpsSample() {
    DWORD now = GetTickCount();
    if (now - lastFpsMs_ < 1000) return;
    lastFpsMs_ = now;
    GameEntry* g = CurrentGame();
    if (!g || !backend_.IsGameRunning(*g)) return;
    double fps = 0; std::string title;
    if (LaunchBackend::ReadGameFps(*g, fps, title)) {
        fpsHist_.push_back((float)fps);
        if (fpsHist_.size() > 120) fpsHist_.pop_front();
    }
}

int LauncherApp::Run(HINSTANCE h) {
    EnablePerMonitorDpi();
    cfg_.Load();
    lib_.Rescan();
    gfx_.Refresh();
    CreateDirectoryA(LauncherConfig::ConfigDir().c_str(), nullptr);

    if (!CreateMain(h)) return 1;
    mainCtx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(mainCtx_);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    RebuildFonts(mainCtx_, dpi_);
    ImGui_ImplWin32_Init(mainHwnd_);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad; // our XInput feed
    if (!mainPlat_.Init(mainHwnd_)) return 1;
    ImGui_ImplDX11_Init(mainPlat_.Device(), mainPlat_.Ctx());
    covers_ = new CoverCache(mainPlat_.Device());

    ShowWindow(mainHwnd_, SW_SHOW);
    UpdateWindow(mainHwnd_);

    // Overlay thread owns its window/context/loop (system-wide F1).
    ovShared_.app = this;
    ovShared_.visible = 0;
    ovShared_.exit = 0;
    ovShared_.hwnd = nullptr;
    ovShared_.ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    ovThread_ = CreateThread(nullptr, 0, OverlayThreadProc, &ovShared_, 0, nullptr);
    WaitForSingleObject(ovShared_.ready, 10000);

    AppendLog(LauncherConfig::LogPath(), "launcher: started (ImGui/DX11)");
    input_.Poll();
    MSG msg{};
    while (!quit_) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (quit_) break;
        }
        if (quit_) break;
        DWORD now = GetTickCount();
        if (now - lastPadMs_ > 16) { lastPadMs_ = now; input_.Poll(); }
        // App-level gamepad shortcuts (tabs/back/menu/view).
        auto nav = input_.ConsumeNav();
        if (nav.type == InputManager::NavEvent::TabLeft) tab_ = (tab_ + 6) % 7;
        else if (nav.type == InputManager::NavEvent::TabRight) tab_ = (tab_ + 1) % 7;
        else if (nav.type == InputManager::NavEvent::Back) { if (tab_ != 0) tab_ = 0; }
        else if (nav.type == InputManager::NavEvent::Menu) tab_ = 2;
        else if (nav.type == InputManager::NavEvent::View) tab_ = 4;
        PollFpsSample();
        if (IsIconic(mainHwnd_)) { Sleep(30); continue; }
        FrameMain();
    }

    // Stop overlay thread.
    InterlockedExchange(&ovShared_.exit, 1);
    if (ovShared_.hwnd) PostMessageA(ovShared_.hwnd, WM_APP + 2, 0, 0);
    WaitForSingleObject(ovThread_, 8000);
    CloseHandle(ovThread_);
    ovThread_ = nullptr;
    CloseHandle(ovShared_.ready);

    backend_.StopMonitor();
    ImGui::SetCurrentContext(mainCtx_);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(mainCtx_);
    mainCtx_ = nullptr;
    delete covers_;
    covers_ = nullptr;
    return 0;
}

void LauncherApp::FrameMain() {
    ImGui::SetCurrentContext(mainCtx_);
    if (!mainPlat_.BeginFrame()) return; // minimized
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    FeedGamepad(mainCtx_);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    const float dpi = dpi_;
    // Custom dark titlebar (the OS frame is borderless).
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * dpi, 6 * dpi));
        ImGui::Text("  AnyPS5");
        ImGui::SameLine();
        ImGui::TextDisabled("Windows Gaming Launcher");
        const float bw = 46 * dpi;
        ImGui::SameLine(ImGui::GetWindowWidth() - bw * 3 - 8 * dpi);
        if (ImGui::Button("_", ImVec2(bw, 30 * dpi))) ShowWindow(mainHwnd_, SW_MINIMIZE);
        ImGui::SameLine(0, 4 * dpi);
        bool maxed = IsZoomed(mainHwnd_) != 0;
        if (ImGui::Button("[ ]", ImVec2(bw, 30 * dpi))) ShowWindow(mainHwnd_, maxed ? SW_RESTORE : SW_MAXIMIZE);
        ImGui::SameLine(0, 4 * dpi);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.14f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.18f, 0.15f, 1.0f));
        if (ImGui::Button("X", ImVec2(bw, 30 * dpi))) { quit_ = true; PostMessageA(mainHwnd_, WM_CLOSE, 0, 0); }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        ImGui::Separator();
    }
    // Sidebar + content.
    ImGui::BeginChild("##side", ImVec2(230 * dpi, 0), true);
    theme::SectionTitle("AnyPS5");
    for (int i = 0; i < 7; ++i) {
        bool sel = (tab_ == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.063f, 0.486f, 0.063f, 1.0f));
        if (ImGui::Button(kTabNames[i], ImVec2(-1, 44 * dpi))) tab_ = i;
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("F1 — меню поверх игры");
    ImGui::TextDisabled(input_.AnyConnected() ? ("Геймпад: " + input_.Primary().api).c_str() : "Геймпад: нет");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    switch (tab_) {
        case 0: DrawHome(dpi); break;
        case 1: DrawLibrary(dpi); break;
        case 2: DrawDisplay(dpi); break;
        case 3: DrawGamepad(dpi); break;
        case 4: DrawDiag(dpi); break;
        case 5: DrawPerf(dpi); break;
        default: DrawSettings(dpi); break;
    }
    ImGui::EndChild();
    ImGui::End();

    theme::DrawToasts(dpi);
    ImGui::Render();
    mainPlat_.EndFrame();
}

// ---------------------------------------------------------------- actions
void LauncherApp::ActionLaunch() {
    GameEntry* g = CurrentGame();
    if (!g) { theme::PushToast("Нет игр — добавьте папку в Библиотеке", true); return; }
    cfg_.selectedGame = g->id;
    GameStatus st = lib_.Status(*g);
    if (!st.exePresent) { theme::PushToast("app.exe не найден: " + g->path, true); return; }
    if (st.prxCount == 0) { theme::PushToast("В libs/ нет .prx — запуск невозможен", true); return; }
    if (backend_.IsGameRunning(*g)) { backend_.FocusGame(*g); theme::PushToast("Игра уже запущена — окно выведено вперёд"); return; }
    if (backend_.Launch(cfg_, *g)) theme::PushToast("Запуск: " + g->name);
    else theme::PushToast("Не удалось запустить (см. журнал)", true);
}

void LauncherApp::ActionAddGame() {
    BROWSEINFOW bi{};
    bi.hwndOwner = mainHwnd_;
    bi.lpszTitle = L"Выберите папку игры (с app.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    char narrow[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
    if (!lib_.AddFolder(narrow)) { theme::PushToast("В папке нет app.exe", true); return; }
    cfg_.selectedGame = GameLibrary::BaseName(narrow);
    cfg_.Save();
    theme::PushToast("Добавлено: " + cfg_.selectedGame);
}

void LauncherApp::ActionRemoveGame(const std::string& id) {
    GameEntry* g = cfg_.FindGame(id);
    if (g && backend_.IsGameRunning(*g)) { theme::PushToast("Игра запущена — сначала закройте её", true); return; }
    lib_.Remove(id);
    if (cfg_.selectedGame == id) cfg_.selectedGame.clear();
    cfg_.Save();
}

void LauncherApp::ActionApplyDisplay(bool perGame) {
    extern int g_editW, g_editH, g_editMode, g_editScaling;
    extern char g_editGpu[128];
    if (perGame) {
        GameEntry* g = CurrentGame();
        if (!g) return;
        g->useGlobalDisplay = false;
        g->display.width = g_editW; g->display.height = g_editH;
        static const char* modes[3] = {"windowed", "borderless", "fullscreen"};
        static const char* sc[3] = {"fit", "fill", "integer"};
        g->display.windowMode = modes[g_editMode];
        g->display.scaling = sc[g_editScaling];
        g->display.gpu = g_editGpu;
        theme::PushToast("Профиль сохранён: " + g->name);
    } else {
        static const char* modes[3] = {"windowed", "borderless", "fullscreen"};
        static const char* sc[3] = {"fit", "fill", "integer"};
        cfg_.display.width = g_editW; cfg_.display.height = g_editH;
        cfg_.display.windowMode = modes[g_editMode];
        cfg_.display.scaling = sc[g_editScaling];
        cfg_.display.gpu = g_editGpu;
        theme::PushToast("Общие настройки изображения сохранены");
    }
    cfg_.Save();
}

void LauncherApp::ActionRestoreDefaults() {
    cfg_.display = DisplayProfile{};
    cfg_.input = InputProfile{};
    cfg_.Save();
    theme::PushToast("Настройки сброшены");
}

void LauncherApp::ActionRumbleTest() {
    if (!input_.AnyConnected()) { theme::PushToast("Геймпад не подключен", true); return; }
    if (input_.Primary().api != "XInput") {
        theme::PushToast("На этом устройстве (WinMM) вибрация проверяется только в игре", true);
        return;
    }
    if (!input_.RumbleTest(800)) theme::PushToast("Вибрация недоступна", true);
    else theme::PushToast("Вибрация: OK");
}

void LauncherApp::ActionSaveInput() {
    extern int g_editDeadzone;
    extern bool g_editRumble, g_editSwap;
    cfg_.input.deadzone = ClampInt(g_editDeadzone, 0, 64);
    cfg_.input.rumble = g_editRumble;
    cfg_.input.swapAB = g_editSwap;
    GameEntry* g = CurrentGame();
    if (g && !g->useGlobalInput) g->input = cfg_.input;
    cfg_.Save();
    theme::PushToast("Настройки управления сохранены");
}

void LauncherApp::FocusGame() {
    GameEntry* g = CurrentGame();
    if (g) backend_.FocusGame(*g);
}

} // namespace launcher
