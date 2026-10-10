#define IMGUI_DEFINE_MATH_OPERATORS
#include "LauncherApp.hpp"
#include "Theme.hpp"
#include "Win32Util.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <shlobj.h>
#include <cstdio>

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace launcher {
// Editor state (per selected game, refreshed when selection changes).
int g_editW = 1920, g_editH = 1080, g_editMode = 0, g_editScaling = 0;
char g_editGpu[128] = {};
int g_editDeadzone = 10;
bool g_editRumble = true, g_editSwap = false;
static std::string g_loadedFor;

static void SyncEditors(LauncherConfig& cfg, GameEntry* g) {
    std::string key = g ? g->id : std::string("@global");
    if (key == g_loadedFor) return;
    g_loadedFor = key;
    const DisplayProfile& d = (g && !g->useGlobalDisplay) ? g->display : cfg.display;
    const InputProfile& in = (g && !g->useGlobalInput) ? g->input : cfg.input;
    g_editW = d.width; g_editH = d.height;
    g_editMode = d.windowMode == "borderless" ? 1 : d.windowMode == "fullscreen" ? 2 : 0;
    g_editScaling = d.scaling == "fill" ? 1 : d.scaling == "integer" ? 2 : 0;
    snprintf(g_editGpu, sizeof(g_editGpu), "%s", d.gpu.c_str());
    g_editDeadzone = in.deadzone; g_editRumble = in.rumble; g_editSwap = in.swapAB;
}

void LauncherApp::DrawHome(float dpi) {
    GameEntry* g = CurrentGame();
    SyncEditors(cfg_, g);
    theme::SectionTitle("Главная");
    if (!g) {
        ImGui::TextWrapped("Библиотека пуста. Откройте вкладку «Библиотека» и добавьте папку игры с app.exe.");
        if (theme::BigButton("Открыть библиотеку", ImVec2(320 * dpi, 64 * dpi))) tab_ = 1;
        return;
    }
    GameStatus st = lib_.Status(*g);
    bool running = backend_.IsGameRunning(*g);
    // Backdrop banner.
    const char* backs[] = {"pic1.png", "pic0.png", "cover.jpg", "icon0.png"};
    std::string bp = FindArt(g->path, backs, 4);
    auto* btex = covers_->Get(bp, 1280);
    if (btex) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float bw = ImGui::GetContentRegionAvail().x;
        float bh = 190 * dpi;
        ImGui::Image((ImTextureID)btex, ImVec2(bw, bh), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0.42f, 0.42f, 0.47f, 1.0f));
        ImGui::SetCursorScreenPos(pos + ImVec2(18 * dpi, 18 * dpi));
        ImGui::BeginGroup();
    }
    ImGui::Text("%s", g->name.c_str());
    ImGui::TextDisabled("%s", g->path.c_str());
    if (btex) ImGui::EndGroup();
    ImGui::Spacing();
    theme::Chip(st.exePresent && st.prxCount > 0 ? "Relink: готов" : "Relink: не готов", st.exePresent && st.prxCount > 0);
    ImGui::SameLine();
    theme::Chip(input_.AnyConnected() ? ("Геймпад: " + input_.Primary().api).c_str() : "Геймпад: нет", input_.AnyConnected());
    ImGui::SameLine();
    auto d = LaunchBackend::EffDisplay(cfg_, *g);
    char mode[96];
    snprintf(mode, sizeof(mode), "%s %dx%d · %s", d.windowMode.c_str(), d.width, d.height, d.scaling.c_str());
    theme::Chip(mode, true);
    ImGui::Spacing();
    if (theme::BigButton(running ? "Вернуться в игру" : "Запустить", ImVec2(320 * dpi, 68 * dpi))) ActionLaunch();
    ImGui::SameLine();
    if (ImGui::Button("Настройки изображения", ImVec2(280 * dpi, 68 * dpi))) tab_ = 2;
    ImGui::Spacing();
    double fps = 0; std::string title;
    if (running && LaunchBackend::ReadGameFps(*g, fps, title)) {
        char f[64]; snprintf(f, sizeof(f), "FPS игры: %.1f", fps);
        theme::StatRow("Состояние", f);
    } else {
        theme::StatRow("Состояние", running ? "игра запущена" : "игра не запущена");
    }
    theme::StatRow("GPU", gfx_.gpus.empty() ? "—" : gfx_.gpus[0].name.c_str());
}

void LauncherApp::DrawLibrary(float dpi) {
    theme::SectionTitle("Библиотека");
    if (theme::BigButton("Добавить папку", ImVec2(280 * dpi, 56 * dpi))) ActionAddGame();
    ImGui::Spacing();
    float cardW = 300 * dpi, cardH = 200 * dpi;
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = avail > cardW + 40 * dpi ? (int)(avail / (cardW + 16 * dpi)) : 1;
    if (cols < 1) cols = 1;
    int n = 0;
    for (auto& game : cfg_.games) {
        if (n % cols) ImGui::SameLine(0, 16 * dpi);
        ImGui::BeginChild(("##gc" + game.id).c_str(), ImVec2(cardW, cardH), true);
        const char* covers[] = {"cover.jpg", "cover.png", "icon0.png"};
        auto* tex = covers_->Get(FindArt(game.path, covers, 3), 512);
        if (tex) {
            ImGui::Image((ImTextureID)tex, ImVec2(cardW - 24 * dpi, 110 * dpi));
        } else {
            // Procedural fallback: monogram tile.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, p + ImVec2(cardW - 24 * dpi, 110 * dpi), IM_COL32(24, 90, 24, 255), 8 * dpi);
            ImGui::SetCursorScreenPos(p + ImVec2(12 * dpi, 28 * dpi));
            ImGui::Text("%s", game.name.substr(0, 2).c_str());
            ImGui::Dummy(ImVec2(0, 110 * dpi - 60 * dpi));
        }
        bool sel = (cfg_.selectedGame == game.id);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.95f, 0.65f, 1.0f));
        if (ImGui::Selectable(game.name.c_str(), sel, 0, ImVec2(cardW - 24 * dpi, 0))) {
            cfg_.selectedGame = game.id;
            g_loadedFor.clear();
            cfg_.Save();
        }
        if (sel) ImGui::PopStyleColor();
        GameStatus st = lib_.Status(game);
        ImGui::TextDisabled("libs: %d .prx", st.prxCount);
        ImGui::EndChild();
        ++n;
    }
    if (cfg_.games.empty()) ImGui::TextDisabled("Пока пусто — нажмите «Добавить папку».");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    GameEntry* g = CurrentGame();
    if (g) {
        ImGui::Text("Выбрано: %s", g->name.c_str());
        ImGui::TextDisabled("%s", g->path.c_str());
        if (ImGui::Button("Профиль игры…")) { snprintf(profileGame_, sizeof(profileGame_), "%s", g->id.c_str()); ImGui::OpenPopup("Профиль игры"); }
        ImGui::SameLine();
        if (ImGui::Button("Убрать из списка")) ActionRemoveGame(g->id);
        if (ImGui::BeginPopupModal("Профиль игры", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            GameEntry* pg = cfg_.FindGame(profileGame_);
            if (pg) {
                bool ugd = pg->useGlobalDisplay, ugi = pg->useGlobalInput;
                if (ImGui::Checkbox("Свои настройки изображения", &ugd)) {}
                pg->useGlobalDisplay = !ugd ? true : false;
                ImGui::TextDisabled("(вкл = общие, выкл = свои)");
                if (ImGui::Checkbox("Свои настройки управления", &ugi)) {}
                pg->useGlobalInput = !ugi ? true : false;
                if (ImGui::Button("Сохранить и закрыть")) { cfg_.Save(); g_loadedFor.clear(); ImGui::CloseCurrentPopup(); }
            } else ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}

void LauncherApp::DrawDisplay(float dpi) {
    theme::SectionTitle("Изображение");
    GameEntry* g = CurrentGame();
    SyncEditors(cfg_, g);
    ImGui::Text("Разрешение окна (Full HD по умолчанию)");
    ImGui::SetNextItemWidth(140 * dpi);
    ImGui::InputInt("Ширина##w", &g_editW, 0, 0);
    g_editW = ClampInt(g_editW, 320, 7680);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * dpi);
    ImGui::InputInt("Высота##h", &g_editH, 0, 0);
    g_editH = ClampInt(g_editH, 180, 4320);
    ImGui::Spacing();
    ImGui::Text("Режим окна");
    const char* modes[3] = {"Оконный", "Без рамки", "Полный экран"};
    const char* mids[3] = {"windowed", "borderless", "fullscreen"};
    g_editMode = theme::Segmented("##mode", modes, 3, g_editMode);
    ImGui::Spacing();
    ImGui::Text("Масштабирование (16:9, без растяжения — кроме Fill)");
    const char* scs[3] = {"Fit", "Fill", "Integer"};
    g_editScaling = theme::Segmented("##scaling", scs, 3, g_editScaling);
    ImGui::TextDisabled("Fit — вписать с полями · Fill — растянуть · Integer — целый масштаб, чёткие пиксели");
    ImGui::Spacing();
    ImGui::Text("Графический адаптер (пусто = авто)");
    if (ImGui::BeginCombo("##gpu", g_editGpu[0] ? g_editGpu : "(авто)")) {
        if (ImGui::Selectable("(авто)", !g_editGpu[0])) g_editGpu[0] = '\0';
        for (auto& gpu : gfx_.gpus) {
            char label[160];
            snprintf(label, sizeof(label), "%s [%s]", gpu.name.c_str(), gpu.type.c_str());
            if (ImGui::Selectable(label, gpu.name == g_editGpu)) snprintf(g_editGpu, sizeof(g_editGpu), "%s", gpu.name.c_str());
        }
        ImGui::EndCombo();
    }
    if (g) {
        bool per = !g->useGlobalDisplay;
        if (ImGui::Checkbox("Индивидуальный профиль для этой игры", &per)) g->useGlobalDisplay = !per;
    }
    ImGui::Spacing();
    if (theme::BigButton("Применить", ImVec2(240 * dpi, 56 * dpi))) ActionApplyDisplay(g && !g->useGlobalDisplay);
    ImGui::SameLine();
    if (ImGui::Button("Сбросить", ImVec2(180 * dpi, 56 * dpi))) { ActionRestoreDefaults(); g_loadedFor.clear(); }
    ImGui::Spacing();
    ImGui::TextDisabled("VSync: всегда ВКЛ (FIFO + ожидание vblank ~60 Гц). Ограничение FPS задаёт vblank движка.");
    ImGui::TextDisabled("Применяется при следующем запуске игры. Alt+Tab и сворачивание обрабатываются движком (пауза подачи).");
    (void)mids;
}

static void StickVisual(const char* id, int x, int y, float dpi) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 46 * dpi;
    dl->AddCircle(p + ImVec2(r, r), r, IM_COL32(120, 130, 150, 255), 32, 2 * dpi);
    dl->AddCircleFilled(p + ImVec2(r, r), 4 * dpi, IM_COL32(120, 130, 150, 255));
    float nx = x / 32767.0f, ny = -y / 32767.0f;
    ImVec2 knob = p + ImVec2(r + nx * (r - 8 * dpi), r + ny * (r - 8 * dpi));
    bool active = (x * x + y * y) > 8000 * 8000;
    dl->AddCircleFilled(knob, 12 * dpi, active ? IM_COL32(60, 200, 60, 255) : IM_COL32(90, 98, 115, 255));
    ImGui::Dummy(ImVec2(r * 2, r * 2 + 6 * dpi));
    ImGui::TextDisabled("%s (%d, %d)", id, x, y);
}

void LauncherApp::DrawGamepad(float dpi) {
    theme::SectionTitle("Геймпад");
    input_.Poll();
    const PadState& p = input_.Primary();
    if (!p.connected) {
        ImGui::TextWrapped("Геймпад не найден. Вставьте USB-свисток китайского Xbox-контроллера и включите геймпад. Поддерживаются XInput (предпочтительно) и WinMM/DirectInput.");
    } else {
        char s[128]; snprintf(s, sizeof(s), "Подключено: %s (слот %d)", p.api.c_str(), p.slot);
        theme::Chip(s, true);
    }
    ImGui::Spacing();
    // Live buttons.
    auto lamp = [&](const char* name, bool on) {
        ImU32 c = on ? IM_COL32(60, 200, 60, 255) : IM_COL32(60, 66, 80, 255);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cur = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(cur + ImVec2(8 * dpi, 8 * dpi), 8 * dpi, c);
        ImGui::Dummy(ImVec2(18 * dpi, 18 * dpi));
        ImGui::SameLine(); ImGui::Text("%s", name);
    };
    ImGui::BeginGroup();
    lamp("A", p.a); lamp("B", p.b); lamp("X", p.x); lamp("Y", p.y);
    ImGui::EndGroup(); ImGui::SameLine(0, 30 * dpi);
    ImGui::BeginGroup();
    lamp("LB", p.lb); lamp("RB", p.rb); lamp("Menu", p.menu); lamp("View", p.view);
    ImGui::EndGroup(); ImGui::SameLine(0, 30 * dpi);
    ImGui::BeginGroup();
    lamp("D-Up", p.dup); lamp("D-Down", p.ddown); lamp("D-Left", p.dleft); lamp("D-Right", p.dright);
    ImGui::EndGroup(); ImGui::SameLine(0, 30 * dpi);
    StickVisual("Левый стик", p.lx, p.ly, dpi);
    ImGui::SameLine(0, 30 * dpi);
    StickVisual("Правый стик", p.rx, p.ry, dpi);
    ImGui::Spacing();
    char trig[96]; snprintf(trig, sizeof(trig), "Триггеры: LT %d / 255 · RT %d / 255", p.lt, p.rt);
    ImGui::TextDisabled("%s", trig);
    ImGui::ProgressBar(p.lt / 255.0f, ImVec2(220 * dpi, 0), "LT");
    ImGui::SameLine(); ImGui::ProgressBar(p.rt / 255.0f, ImVec2(220 * dpi, 0), "RT");
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    GameEntry* g = CurrentGame();
    SyncEditors(cfg_, g);
    ImGui::Text("Мёртвая зона стиков: %d", g_editDeadzone);
    ImGui::SliderInt("##dz", &g_editDeadzone, 0, 64);
    bool r = g_editRumble, sw = g_editSwap;
    if (theme::Toggle("##rum", &r)) g_editRumble = r;
    ImGui::SameLine(); ImGui::Text("Вибрация");
    ImGui::SameLine(0, 30 * dpi);
    if (theme::Toggle("##swap", &sw)) g_editSwap = sw;
    ImGui::SameLine(); ImGui::Text("Поменять A/B");
    ImGui::Spacing();
    if (ImGui::Button("Проверить вибрацию", ImVec2(260 * dpi, 48 * dpi))) ActionRumbleTest();
    ImGui::SameLine();
    if (theme::BigButton("Сохранить", ImVec2(220 * dpi, 48 * dpi))) ActionSaveInput();
    ImGui::Spacing();
    ImGui::TextWrapped("Схема: %s", InputManager::SchemeText(g_editSwap));
    ImGui::TextDisabled("Навигация лаунчера: D-pad/стик — движение, A — подтвердить, B — назад, LB/RB — вкладки, Menu — изображение, View — диагностика.");
}

void LauncherApp::DrawDiag(float dpi) {
    theme::SectionTitle("Диагностика");
    if (ImGui::Button("Обновить", ImVec2(180 * dpi, 44 * dpi))) { gfx_.Refresh(); lib_.Rescan(); }
    ImGui::Spacing();
    ImGui::TextDisabled("Исполнение: нативно на ПК (Relink). Игра PS5 не задействована — рендеринг выполняет GPU ПК через Vulkan.");
    GameEntry* g = CurrentGame();
    if (g) {
        GameStatus st = lib_.Status(*g);
        char b[256];
        snprintf(b, sizeof(b), "%s | app.exe: %s | libs: %d .prx", g->name.c_str(), st.exePresent ? "есть" : "НЕТ", st.prxCount);
        theme::StatRow("Игра", b, st.exePresent && st.prxCount > 0);
        bool running = backend_.IsGameRunning(*g);
        theme::StatRow("Запущена", running ? "да" : "нет", true);
        double fps = 0; std::string title;
        if (running && LaunchBackend::ReadGameFps(*g, fps, title)) {
            char f[96]; snprintf(f, sizeof(f), "%.1f (кадр %.1f мс)", fps, 1000.0 / fps);
            theme::StatRow("FPS", f);
            theme::StatRow("Заголовок окна", title.c_str());
        }
    }
    theme::StatRow("Контроллер", input_.AnyConnected() ? (input_.Primary().api + " слот " + std::to_string(input_.Primary().slot)).c_str() : "не подключен", input_.AnyConnected());
    theme::StatRow("API рендеринга", "Vulkan 1.1+ (libSceAgcDriver → swapchain в окне Windows)");
    if (!gfx_.vulkanAvailable) theme::StatRow("Vulkan", gfx_.loaderError.c_str(), false);
    else for (auto& gpu : gfx_.gpus) theme::StatRow("GPU", (gpu.name + " [" + gpu.type + "] API " + gpu.api).c_str());
    theme::StatRow("Present", GraphicsDiag::PresentModeText());
    theme::StatRow("VSync", GraphicsDiag::VsyncText());
    ImGui::Spacing();
    ImGui::Text("Журнал последнего запуска:");
    std::string log = backend_.LastRunLog();
    std::string tail = "(запусков пока не было)";
    if (!log.empty()) {
        HANDLE h = CreateFileA(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, nullptr);
            DWORD off = sz > 6000 ? sz - 6000 : 0;
            SetFilePointer(h, off, nullptr, FILE_BEGIN);
            char* buf = new char[6001]();
            DWORD n = 0;
            ReadFile(h, buf, 6000, &n, nullptr);
            CloseHandle(h);
            tail.assign(buf, n);
            delete[] buf;
        } else tail = "(лог недоступен)";
    }
    ImGui::BeginChild("##log", ImVec2(0, 180 * dpi), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(tail.c_str());
    ImGui::EndChild();
}

void LauncherApp::DrawPerf(float dpi) {
    theme::SectionTitle("Производительность");
    if (fpsHist_.empty()) {
        ImGui::TextDisabled("Нет данных — запустите игру: FPS считывается из заголовка её окна раз в секунду.");
    } else {
        static float data[120] = {};
        int n = (int)fpsHist_.size();
        for (int i = 0; i < n; ++i) data[i] = fpsHist_[(fpsHist_.size() - n) + i];
        char ov[64]; snprintf(ov, sizeof(ov), "FPS: %.1f", data[n - 1]);
        ImGui::PlotLines("##fps", data, n, 0, ov, 0.0f, 70.0f, ImVec2(-1, 160 * dpi));
        ImGui::TextDisabled("vblank движка ~60 Гц; просадки ниже — нагрузка GPU/CPU или шейдерная компиляция.");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Детальные тайминги кадра: соберите движок с APS5_ENABLE_TIMING_LOG=ON (frame-timing.log).");
    (void)dpi;
}

void LauncherApp::DrawSettings(float dpi) {
    theme::SectionTitle("Общие настройки");
    char dir[MAX_PATH] = {};
    snprintf(dir, sizeof(dir), "%s", cfg_.gamesDir.c_str());
    ImGui::Text("Папка игр");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##gamesdir", dir, sizeof(dir), ImGuiInputTextFlags_EnterReturnsTrue)) {
        cfg_.gamesDir = dir;
        lib_.Rescan();
        cfg_.Save();
        theme::PushToast("Папка игр сохранена");
    }
    ImGui::Spacing();
    if (ImGui::Button("Открыть папку конфигов", ImVec2(300 * dpi, 48 * dpi))) {
        ShellExecuteA(nullptr, "open", LauncherConfig::ConfigDir().c_str(), nullptr, nullptr, SW_SHOW);
    }
    ImGui::SameLine();
    if (ImGui::Button("Пересканировать библиотеку", ImVec2(320 * dpi, 48 * dpi))) { lib_.Rescan(); cfg_.Save(); }
    ImGui::Spacing();
    ImGui::TextDisabled("Лаунчер должен оставаться запущенным (можно свернуть), чтобы F1-меню работало поверх игры.");
    ImGui::TextDisabled("Закрытие лаунчера не закрывает игру.");
    (void)dpi;
}

// ---------------------------------------------------------------- overlay
// Own thread: own borderless topmost window, D3D11 device, ImGui context.
// System-wide F1 hotkey fires even while the game is focused. The game keeps
// rendering (SDL reads the pad via background events); keyboard focus moves
// to the panel while it is open and returns to the game on close.
namespace {
constexpr UINT kOvShow = WM_APP + 1;
constexpr UINT kOvExit = WM_APP + 2;

struct OvThread {
    LauncherApp::OverlayShared* sh = nullptr;
    HWND hwnd = nullptr;
    ImGuiContext* ctx = nullptr;
    PlatformD3D11 plat;
    InputManager input;
    LauncherConfig cfg; // private copy, reloaded on every show
    GraphicsDiag gfx;
    float dpi = 1.0f;
    int ovMode = 0, ovScaling = 0;
    bool escPrev = false;
};

OvThread* OvOf(HWND h) { return reinterpret_cast<OvThread*>(GetWindowLongPtrA(h, GWLP_USERDATA)); }

void OvFeedPad(OvThread* o) {
    ImGui::SetCurrentContext(o->ctx);
    ImGuiIO& io = ImGui::GetIO();
    const PadState& pad = o->input.Primary();
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
}

void OvHide(OvThread* o, bool focusGame) {
    InterlockedExchange(&o->sh->visible, 0);
    ShowWindow(o->hwnd, SW_HIDE);
    KillTimer(o->hwnd, 1);
    if (focusGame) {
        GameEntry* g = o->cfg.FindGame(o->cfg.selectedGame);
        if (!g && !o->cfg.games.empty()) g = &o->cfg.games[0];
        if (g) {
            LaunchBackend tmp([](const std::string&) {});
            tmp.FocusGame(*g);
        }
    }
}

void OvShow(OvThread* o) {
    o->cfg.Load(); // fresh snapshot (main thread owns the file otherwise)
    GameLibrary lib(o->cfg);
    lib.Rescan();
    GameEntry* g = o->cfg.FindGame(o->cfg.selectedGame);
    if (!g && !o->cfg.games.empty()) g = &o->cfg.games[0];
    const DisplayProfile& d = g && !g->useGlobalDisplay ? g->display : o->cfg.display;
    o->ovMode = d.windowMode == "borderless" ? 1 : d.windowMode == "fullscreen" ? 2 : 0;
    o->ovScaling = d.scaling == "fill" ? 1 : d.scaling == "integer" ? 2 : 0;
    o->gfx.Refresh();
    o->dpi = DpiScaleForWindow(o->hwnd);
    ImGui::SetCurrentContext(o->ctx);
    theme::LoadFonts(o->dpi);
    theme::ApplyGraphiteStyle(o->dpi);
    InterlockedExchange(&o->sh->visible, 1);
    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    AnimateWindow(o->hwnd, 160, AW_BLEND);
    SetForegroundWindow(o->hwnd);
    SetTimer(o->hwnd, 1, 16, nullptr);
}

void OvRender(OvThread* o) {
    ImGui::SetCurrentContext(o->ctx);
    if (!o->plat.BeginFrame()) return;
    o->input.Poll();
    auto nav = o->input.ConsumeNav();
    if (nav.type == InputManager::NavEvent::Back) {
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) { OvHide(o, true); return; }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    OvFeedPad(o);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##ov", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    const float dpi = o->dpi;
    GameEntry* g = o->cfg.FindGame(o->cfg.selectedGame);
    if (!g && !o->cfg.games.empty()) g = &o->cfg.games[0];
    ImGui::Text("AnyPS5 · меню (F1)");
    ImGui::Separator();
    if (g) {
        ImGui::TextWrapped("%s", g->name.c_str());
        LaunchBackend tmp([](const std::string&) {});
        double fps = 0; std::string title;
        if (tmp.IsGameRunning(*g) && LaunchBackend::ReadGameFps(*g, fps, title)) {
            char f[64]; snprintf(f, sizeof(f), "FPS: %.1f", fps);
            ImGui::Text("%s", f);
        } else ImGui::TextDisabled("игра не запущена");
    } else ImGui::TextDisabled("нет игр");
    const PadState& pad = o->input.Primary();
    ImGui::Text("%s", pad.connected ? ("Геймпад: " + pad.api).c_str() : "Геймпад: нет");
    if (!o->gfx.gpus.empty()) ImGui::TextDisabled("GPU: %s", o->gfx.gpus[0].name.c_str());
    ImGui::Spacing();
    ImGui::Text("Изображение (применится при следующем запуске)");
    const char* modes[3] = {"Оконный", "Без рамки", "Полный экран"};
    const char* scs[3] = {"Fit", "Fill", "Integer"};
    o->ovMode = theme::Segmented("##ovmode", modes, 3, o->ovMode);
    o->ovScaling = theme::Segmented("##ovsc", scs, 3, o->ovScaling);
    if (ImGui::Button("Применить", ImVec2(-1, 44 * dpi))) {
        static const char* mids[3] = {"windowed", "borderless", "fullscreen"};
        static const char* sids[3] = {"fit", "fill", "integer"};
        o->cfg.Load();
        GameEntry* gg = o->cfg.FindGame(o->cfg.selectedGame);
        if (gg) {
            DisplayProfile* d = gg->useGlobalDisplay ? &o->cfg.display : &gg->display;
            d->windowMode = mids[o->ovMode];
            d->scaling = sids[o->ovScaling];
            o->cfg.Save();
        }
    }
    ImGui::Spacing();
    if (theme::BigButton("Вернуться в игру", ImVec2(-1, 52 * dpi))) OvHide(o, true);
    if (ImGui::Button("Показать лаунчер", ImVec2(-1, 44 * dpi))) {
        HWND main = FindWindowA("AnyPS5LauncherUI", nullptr);
        OvHide(o, false);
        if (main) { ShowWindow(main, SW_RESTORE); SetForegroundWindow(main); }
    }
    if (ImGui::Button("Закрыть (F1 / B)", ImVec2(-1, 44 * dpi))) OvHide(o, true);
    ImGui::End();

    bool esc = ImGui::IsKeyDown(ImGuiKey_Escape);
    if (esc && !o->escPrev) OvHide(o, true);
    o->escPrev = esc;

    ImGui::Render();
    o->plat.EndFrame();
}

LRESULT CALLBACK OvWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTA*>(l);
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    OvThread* o = OvOf(h);
    if (o && o->ctx) {
        ImGui::SetCurrentContext(o->ctx);
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
    }
    switch (m) {
        case WM_HOTKEY:
            if (w == 7 && o) {
                if (InterlockedCompareExchange(&o->sh->visible, 0, 0)) OvHide(o, true);
                else OvShow(o);
            }
            return 0;
        case WM_TIMER:
            if (o && InterlockedCompareExchange(&o->sh->visible, 0, 0)) OvRender(o);
            return 0;
        case kOvShow:
            if (o) OvShow(o);
            return 0;
        case kOvExit:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
} // namespace

DWORD WINAPI OverlayThreadProc(LPVOID p) {
    auto* sh = static_cast<LauncherApp::OverlayShared*>(p);
    WNDCLASSA wc{};
    wc.lpfnWndProc = OvWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "AnyPS5Overlay";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    OvThread o;
    o.sh = sh;
    RECT work{};
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    const int W = 800, H = 640;
    const int x = work.left + ((work.right - work.left) - W) / 2;
    const int y = work.top + ((work.bottom - work.top) - H) / 2;
    o.hwnd = CreateWindowExA(WS_EX_TOPMOST, "AnyPS5Overlay", "AnyPS5",
        WS_POPUP | WS_CLIPCHILDREN, x, y, W, H, nullptr, nullptr, GetModuleHandle(nullptr), &o);
    if (!o.hwnd) { SetEvent(sh->ready); return 1; }
    SetWindowTitleUTF8(o.hwnd, "AnyPS5 — меню (F1)");
    o.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(o.ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;
    o.dpi = DpiScaleForWindow(o.hwnd);
    theme::LoadFonts(o.dpi);
    theme::ApplyGraphiteStyle(o.dpi);
    ImGui_ImplWin32_Init(o.hwnd);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (!o.plat.Init(o.hwnd)) { SetEvent(sh->ready); return 1; }
    ImGui_ImplDX11_Init(o.plat.Device(), o.plat.Ctx());
    RegisterHotKey(o.hwnd, 7, 0, VK_F1);
    sh->hwnd = o.hwnd;
    SetEvent(sh->ready);
    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == kOvExit) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    UnregisterHotKey(o.hwnd, 7);
    ImGui::SetCurrentContext(o.ctx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(o.ctx);
    o.ctx = nullptr;
    DestroyWindow(o.hwnd);
    sh->hwnd = nullptr;
    return 0;
}

void LauncherApp::ToggleOverlay() {
    if (ovShared_.hwnd) PostMessageA(ovShared_.hwnd, kOvShow, 0, 0);
}

void LauncherApp::HideOverlay() {}

void LauncherApp::DrawOverlayPanel(float dpi) { (void)dpi; }

} // namespace launcher
