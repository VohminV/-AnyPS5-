#pragma once
// Launcher application: ImGui main window (7 screens) + system-wide F1 overlay.
// All visuals drawn by Dear ImGui; Win32 is infrastructure only.
#include "Config.hpp"
#include "GameLibrary.hpp"
#include "LaunchBackend.hpp"
#include "InputManager.hpp"
#include "GraphicsDiag.hpp"
#include "PlatformD3D11.hpp"
#include "Covers.hpp"
#include <windows.h>
#include <deque>

struct ImGuiContext;

namespace launcher {

DWORD WINAPI OverlayThreadProc(LPVOID p); // overlay thread entry (Screens.cpp)

class LauncherApp {
public:
    // Cross-thread overlay state. The overlay thread owns its window, D3D
    // device and ImGui context; the main thread only signals it.
    struct OverlayShared {
        LauncherApp* app = nullptr;
        volatile LONG visible = 0;
        volatile LONG exit = 0;
        HWND hwnd = nullptr;
        HANDLE ready = nullptr;
    };

    LauncherApp();
    ~LauncherApp();
    int Run(HINSTANCE h);

private:
    // --- windows ---
    static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMain(HWND, UINT, WPARAM, LPARAM);
    bool CreateMain(HINSTANCE h);

    // --- frames ---
    void FrameMain();
    void FrameOverlay();
    void FeedGamepad(ImGuiContext* ctx);
    void RebuildFonts(ImGuiContext* ctx, float dpi);

    // --- screens ---
    void DrawSidebar(float dpi);
    void DrawHome(float dpi);
    void DrawLibrary(float dpi);
    void DrawDisplay(float dpi);
    void DrawGamepad(float dpi);
    void DrawDiag(float dpi);
    void DrawPerf(float dpi);
    void DrawSettings(float dpi);
    void DrawOverlayPanel(float dpi);

    // --- actions ---
    void ActionLaunch();
    void ActionAddGame();
    void ActionRemoveGame(const std::string& id);
    void ActionApplyDisplay(bool perGame);
    void ActionRestoreDefaults();
    void ActionRumbleTest();
    void ActionSaveInput();
    void ToggleOverlay();   // main thread -> ask overlay thread to show
    void HideOverlay();     // (overlay thread hides itself; stub for symmetry)
    void FocusGame();
    GameEntry* CurrentGame();
    void PollFpsSample();

    LauncherConfig cfg_;
    GameLibrary lib_;
    LaunchBackend backend_;
    InputManager input_;
    GraphicsDiag gfx_;

    HINSTANCE hinst_ = nullptr;
    HWND mainHwnd_ = nullptr;
    PlatformD3D11 mainPlat_;
    ImGuiContext* mainCtx_ = nullptr;
    CoverCache* covers_ = nullptr;
    float dpi_ = 1.0f;

    int tab_ = 0; // 0 home,1 library,2 display,3 gamepad,4 diag,5 perf,6 settings
    char profileGame_[128] = {}; // modal per-game profile editor target
    bool showAddHelp_ = false;
    std::deque<float> fpsHist_;
    DWORD lastFpsMs_ = 0;
    DWORD lastPadMs_ = 0;
    OverlayShared ovShared_{};
    HANDLE ovThread_ = nullptr;
    bool quit_ = false;
};

} // namespace launcher
