#pragma once
// Win32 infrastructure helpers (window/system only — never UI controls).
// UTF-8 <-> UTF-16 conversion so Russian text is correct everywhere.
#include <windows.h>
#include <string>

namespace launcher {

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

inline void SetWindowTitleUTF8(HWND hwnd, const std::string& title) {
    std::wstring w = Utf8ToWide(title);
    SetWindowTextW(hwnd, w.c_str());
}

inline float DpiScaleForWindow(HWND hwnd) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    using Sig = UINT(WINAPI*)(HWND);
    auto fn = reinterpret_cast<Sig>(GetProcAddress(u32, "GetDpiForWindow"));
    UINT dpi = fn ? fn(hwnd) : 96;
    return (dpi == 0 ? 96 : dpi) / 96.0f;
}

inline void EnablePerMonitorDpi() {
    HMODULE sh = LoadLibraryA("shcore.dll");
    if (sh) {
        using Sig = HRESULT(WINAPI*)(int);
        auto fn = reinterpret_cast<Sig>(GetProcAddress(sh, "SetProcessDpiAwareness"));
        if (fn) fn(2); // PROCESS_PER_MONITOR_DPI_AWARE
        FreeLibrary(sh);
    }
}

} // namespace launcher
