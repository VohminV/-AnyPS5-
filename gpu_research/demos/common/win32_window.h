// Minimal Win32 window helper for the gpu_research demos.
// Header-only. Depends only on <windows.h> and the Vulkan loader at runtime
// (no link-time Vulkan dependency: entry points via GetProcAddress).
// NOT part of the game project. Requires LunarG SDK headers to compile.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace demo {

inline LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

// Creates a visible overlapped window and pumps messages until frames have
// been presented or the user closes it. Returns false when closed early.
class Window {
public:
    Window(const char* title, std::uint32_t width, std::uint32_t height) : width_(width), height_(height) {
        WNDCLASSA klass{};
        klass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        klass.lpfnWndProc = WindowProc;
        klass.hInstance = GetModuleHandleA(nullptr);
        klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
        klass.lpszClassName = "GpuResearchDemo";
        if (!RegisterClassA(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("RegisterClassA failed");
        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, FALSE);
        hwnd_ = CreateWindowExA(0, klass.lpszClassName, title, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, klass.hInstance, nullptr);
        if (hwnd_ == nullptr) throw std::runtime_error("CreateWindowExA failed");
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }

    ~Window() {
        if (hwnd_ != nullptr) DestroyWindow(hwnd_);
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    HWND handle() const { return hwnd_; }

    // Returns false when the user closed the window.
    bool pump() {
        MSG msg{};
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return false;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        return true;
    }

private:
    HWND hwnd_ = nullptr;
    std::uint32_t width_ = 0, height_ = 0;
};

} // namespace demo
