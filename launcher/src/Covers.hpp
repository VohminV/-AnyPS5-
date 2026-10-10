#pragma once
// Game artwork: loads cover/backdrop PNG/JPG via stb_image (already vendored)
// into D3D11 shader-resource views. Cached per path; procedural fallback.
#include <d3d11.h>
#include <string>
#include <vector>

namespace launcher {

class CoverCache {
public:
    explicit CoverCache(ID3D11Device* device) : device_(device) {}
    void SetDevice(ID3D11Device* d) { device_ = d; }
    // Returns SRV or nullptr. maxW caps huge backdrops (pic0 ~15MB PNG).
    ID3D11ShaderResourceView* Get(const std::string& path, int maxW = 1024);
    void Clear();

private:
    ID3D11Device* device_ = nullptr;
    struct Entry {
        std::string path;
        ID3D11ShaderResourceView* srv = nullptr;
        int w = 0, h = 0;
    };
    std::vector<Entry> entries_;
};

std::string FindArt(const std::string& gameDir, const char* const* names, int count);

} // namespace launcher
