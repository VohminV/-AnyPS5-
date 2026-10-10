#include "Covers.hpp"
#include <windows.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#include <algorithm>

namespace launcher {

std::string FindArt(const std::string& gameDir, const char* const* names, int count) {
    static const char* dirs[] = {"\\", "\\sce_sys\\", "\\app0\\", "\\app0\\sce_sys\\"};
    for (auto d : dirs) {
        for (int i = 0; i < count; ++i) {
            std::string p = gameDir + d + names[i];
            DWORD a = GetFileAttributesA(p.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return p;
        }
    }
    return {};
}

ID3D11ShaderResourceView* CoverCache::Get(const std::string& path, int maxW) {
    if (path.empty() || !device_) return nullptr;
    for (auto& e : entries_)
        if (e.path == path) return e.srv;
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) { stbi_image_free(px); return nullptr; }
    stbi_uc* data = px;
    stbi_uc* resized = nullptr;
    bool usedResize = false;
    if (w > maxW) {
        int nh = std::max(1, h * maxW / w);
        resized = new stbi_uc[(size_t)maxW * nh * 4];
        if (stbir_resize_uint8_linear(px, w, h, 0, resized, maxW, nh, 0, STBIR_RGBA)) {
            data = resized;
            w = maxW; h = nh;
            usedResize = true;
        } else {
            delete[] resized;
            resized = nullptr;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{data, (UINT)(w * 4), 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (device_->CreateTexture2D(&td, &init, &tex) == S_OK)
        device_->CreateShaderResourceView(tex, nullptr, &srv);
    if (tex) tex->Release();
    if (usedResize) delete[] resized;
    stbi_image_free(px);
    if (srv) entries_.push_back({path, srv, w, h});
    return srv;
}

void CoverCache::Clear() {
    for (auto& e : entries_)
        if (e.srv) e.srv->Release();
    entries_.clear();
}

} // namespace launcher
