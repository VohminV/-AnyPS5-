// AnyPS5 Windows Gaming Launcher — entry point.
// ImGui + Win32 + DirectX 11. Single instance; closing the window quits
// (the game keeps running). Keep it minimized for system-wide F1 menu.
#include <windows.h>
#include "LauncherApp.hpp"

using launcher::LauncherApp;

int WINAPI WinMain(HINSTANCE h, HINSTANCE, LPSTR, int) {
    HANDLE m = CreateMutexA(nullptr, TRUE, "AnyPS5LauncherSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowA("AnyPS5LauncherUI", nullptr);
        if (w) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
        CloseHandle(m);
        return 0;
    }
    LauncherApp app;
    int rc = app.Run(h);
    ReleaseMutex(m);
    CloseHandle(m);
    return rc;
}
