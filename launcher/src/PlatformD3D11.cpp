#include "PlatformD3D11.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

namespace launcher {

PlatformD3D11::PlatformD3D11() = default;
PlatformD3D11::~PlatformD3D11() { Shutdown(); }

bool PlatformD3D11::Init(HWND hwnd) {
    hwnd_ = hwnd;
    UINT flags = 0;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL out = D3D_FEATURE_LEVEL_11_0;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &out, &ctx_) != S_OK) {
        // Fallback: WARP (always present, honest software path).
        if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                levels, 2, D3D11_SDK_VERSION, &sd, &swap_, &device_, &out, &ctx_) != S_OK)
            return false;
    }
    return createRenderTarget();
}

void PlatformD3D11::Shutdown() {
    cleanupRenderTarget();
    if (swap_) { swap_->Release(); swap_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    hwnd_ = nullptr;
}

bool PlatformD3D11::createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (!swap_ || swap_->GetBuffer(0, IID_PPV_ARGS(&back)) != S_OK) return false;
    bool ok = device_->CreateRenderTargetView(back, nullptr, &rtv_) == S_OK;
    back->Release();
    return ok;
}

void PlatformD3D11::cleanupRenderTarget() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
}

void PlatformD3D11::OnResize(int w, int h) {
    if (!swap_ || w <= 0 || h <= 0) return;
    cleanupRenderTarget();
    swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

bool PlatformD3D11::BeginFrame() {
    if (!device_) return false;
    RECT r{};
    GetClientRect(hwnd_, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false; // minimized: skip (Alt+Tab friendly)
    static int lastW = 0, lastH = 0;
    if ((w != lastW || h != lastH) && (lastW || lastH)) OnResize(w, h);
    lastW = w; lastH = h;
    return true;
}

void PlatformD3D11::EndFrame() {
    const float clear[4] = {0.07f, 0.07f, 0.09f, 1.0f};
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->ClearRenderTargetView(rtv_, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap_->Present(1, 0); // vsync on
}

} // namespace launcher
