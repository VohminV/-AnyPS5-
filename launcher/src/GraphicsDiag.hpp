#pragma once
// Graphics diagnostics: real Vulkan enumeration via vulkan-1.dll
// (no separate renderer; the game renders through libSceAgcDriver/Vulkan).
#include <string>
#include <vector>

namespace launcher {

struct GpuInfo {
    std::string name;
    std::string type; // discrete | integrated | virtual | other
    std::string api;  // e.g. "1.3.280"
    bool presentable = false; // unknown here; engine decides with surface
};

struct GraphicsDiag {
    bool vulkanAvailable = false;
    std::string loaderError;
    std::vector<GpuInfo> gpus;
    void Refresh();
    static const char* PresentModeText(); // what the engine uses
    static const char* VsyncText();
};

} // namespace launcher
