#pragma once
// Minimal D3D11 + Win32 platform layer for Dear ImGui.
// One instance per top-level window (main launcher window, F1 overlay).
// Handles device/swapchain, resize, DPI font rebuild hook.
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <functional>

struct ImGuiContext;

namespace launcher {

class PlatformD3D11 {
public:
    PlatformD3D11();
    ~PlatformD3D11();
    PlatformD3D11(const PlatformD3D11&) = delete;
    PlatformD3D11& operator=(const PlatformD3D11&) = delete;

    bool Init(HWND hwnd);
    void Shutdown();
    bool BeginFrame();   // false = minimized, skip render
    void EndFrame();     // render + present (vsync on)
    void OnResize(int w, int h);
    void InvalidateDeviceObjects() {}

    ID3D11Device* Device() const { return device_; }
    ID3D11DeviceContext* Ctx() const { return ctx_; }
    HWND Hwnd() const { return hwnd_; }
    // Called after fonts/DPI change.
    std::function<void()> onFontsInvalidated;

private:
    bool createRenderTarget();
    void cleanupRenderTarget();

    HWND hwnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
};

} // namespace launcher
