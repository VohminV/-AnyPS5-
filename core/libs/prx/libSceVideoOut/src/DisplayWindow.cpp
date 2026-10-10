#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "SDL_vulkan.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include "SDL_syswm.h"
#include <windows.h>
#include <commctrl.h>
#endif

namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("DisplayWindow: ") + reason);
}

#ifdef _WIN32
constexpr UINT_PTR DisplayWindowSubclassId = 0x41505335u;
#endif

}

DisplayWindow::~DisplayWindow() {
    Destroy();
}

void DisplayWindow::Ensure(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    require(sourceWidth != 0 && sourceHeight != 0, "source extent must be non-zero");
    if (window == nullptr) create(sourceWidth, sourceHeight);
    updateAspectRatio(sourceWidth, sourceHeight);
}

void DisplayWindow::create(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    // Window mode is driven by the launcher via environment (defaults = Full HD windowed):
    //   ANYPS5_WINDOW_MODE = windowed | borderless | fullscreen (default windowed)
    //   ANYPS5_WINDOW_W / ANYPS5_WINDOW_H = requested client size (default 1920x1080)
    // The game image itself keeps 16:9 via the aspect subclass + presentation letterbox.
    const char* modeEnv = std::getenv("ANYPS5_WINDOW_MODE");
    const std::string mode = modeEnv != nullptr ? modeEnv : "windowed";
    const auto parseExtent = [](const char* name, long fallback) {
        const char* raw = std::getenv(name);
        if (raw == nullptr || *raw == '\0') return fallback;
        char* end = nullptr;
        const long value = std::strtol(raw, &end, 10);
        if (end == raw || value < 320 || value > 7680) return fallback;
        return value;
    };
    long reqW = parseExtent("ANYPS5_WINDOW_W", 1920);
    long reqH = parseExtent("ANYPS5_WINDOW_H", 1080);
    // Keep 16:9 when the requested size is not (e.g. smaller monitor): shrink to fit.
    SDL_Rect usable{};
    require(SDL_GetDisplayUsableBounds(0, &usable) == 0, SDL_GetError());
    require(usable.w > 0 && usable.h > 0, "usable display extent must be positive");
    if (reqW > usable.w || reqH > usable.h) {
        const double scale = std::min(static_cast<double>(usable.w) / reqW, static_cast<double>(usable.h) / reqH);
        reqW = static_cast<long>(reqW * scale);
        reqH = static_cast<long>(reqH * scale);
    }
    const auto title = GetAppTitle_nid_postfix();
    AgcDriverLockVulkanLoader_nid_postfix();
    if (mode == "fullscreen") {
        const auto initialSize = AgcDriver::ComputeContainSize_nid_postfix(sourceWidth, sourceHeight,
            static_cast<std::uint32_t>(usable.w), static_cast<std::uint32_t>(usable.h), true);
        window = SDL_CreateWindow(title.value, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(initialSize.width), static_cast<int>(initialSize.height),
            SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_FULLSCREEN_DESKTOP);
        AgcDriverUnlockVulkanLoader_nid_postfix();
        require(window != nullptr, SDL_GetError());
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0) {
            require(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0, SDL_GetError());
        }
    } else if (mode == "borderless") {
        window = SDL_CreateWindow(title.value, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            usable.w, usable.h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS);
        AgcDriverUnlockVulkanLoader_nid_postfix();
        require(window != nullptr, SDL_GetError());
        SDL_SetWindowPosition(window, usable.x, usable.y);
    } else {
        // windowed (default): Full HD 1920x1080 when it fits, else scaled 16:9 fit.
        const auto initialSize = AgcDriver::ComputeContainSize_nid_postfix(sourceWidth, sourceHeight,
            static_cast<std::uint32_t>(reqW), static_cast<std::uint32_t>(reqH), true);
        require(initialSize.width >= DisplayWindowMinimumWidth && initialSize.height >= DisplayWindowMinimumHeight, "initial window extent is smaller than the minimum");
        window = SDL_CreateWindow(title.value, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(initialSize.width), static_cast<int>(initialSize.height),
            SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        AgcDriverUnlockVulkanLoader_nid_postfix();
        require(window != nullptr, SDL_GetError());
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(DisplayWindowMinimumWidth), static_cast<int>(DisplayWindowMinimumHeight));
    SDL_RaiseWindow(window);
    installSubclass();
}

void DisplayWindow::updateAspectRatio(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    aspectWidth = sourceWidth;
    aspectHeight = sourceHeight;
}

void DisplayWindow::Destroy() noexcept {
    if (window == nullptr) return;
    removeSubclass();
    SDL_DestroyWindow(window);
    window = nullptr;
}

SDL_Window* DisplayWindow::Handle() const {
    return window;
}

void DisplayWindow::ToggleFullscreen() {
    require(window != nullptr, "window must exist before toggling fullscreen");
    const auto flags = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0 ? 0u : static_cast<Uint32>(SDL_WINDOW_FULLSCREEN_DESKTOP);
    require(SDL_SetWindowFullscreen(window, flags) == 0, SDL_GetError());
}

void DisplayWindow::DrawableSize(std::uint32_t& width, std::uint32_t& height) const {
    if (window == nullptr) {
        width = 0;
        height = 0;
        return;
    }
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
    width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
    height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
}

void DisplayWindow::UpdateTitle() {
    require(window != nullptr, "window must exist before updating title");
    static const AppTitle title = GetAppTitle_nid_postfix();
    static std::uint64_t fpsStart = sceKernelGetProcessTimeCounter();
    static std::uint64_t frameNum = 0;
    static std::uint64_t fpsFrames = 0;
    static double currentFps = 0.0;
    const auto now = sceKernelGetProcessTimeCounter();
    const auto frequency = sceKernelGetProcessTimeCounterFrequency();
    frameNum++;
    fpsFrames++;
    if (now - fpsStart >= frequency * 2) {
        currentFps = static_cast<double>(fpsFrames) * static_cast<double>(frequency) / static_cast<double>(now - fpsStart);
        fpsStart = now;
        fpsFrames = 0;
    }
    char text[512];
    // Gamepad-only header: title + FPS, no keyboard hints (no F1/Keys line).
    std::snprintf(text, sizeof(text), "%s | FPS: %.2f", title.value, currentFps);
    // The title feeds OBS window matching: calling SDL_SetWindowTitle on every
    // present made it churn each frame (it used to carry a frame counter),
    // forcing WGC captures to re-initialize (black screen plus an ever-new
    // entry in OBS's window list) and costing a Win32 round-trip per frame.
    // Refresh at most twice a second and only when the readout changed.
    static std::uint64_t lastSet = 0;
    static char lastText[512] = "";
    if (now - lastSet >= frequency / 2 && std::strcmp(text, lastText) != 0) {
        SDL_SetWindowTitle(window, text);
        std::strcpy(lastText, text);
        lastSet = now;
    }
}

void DisplayWindow::installSubclass() {
#ifdef _WIN32
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    require(SDL_GetWindowWMInfo(window, &info) == SDL_TRUE, SDL_GetError());
    require(info.subsystem == SDL_SYSWM_WINDOWS, "unsupported window subsystem");
    const auto attached = SetWindowSubclass(info.info.win.window, reinterpret_cast<SUBCLASSPROC>(&DisplayWindow::windowProc), DisplayWindowSubclassId, reinterpret_cast<DWORD_PTR>(this));
    require(attached != FALSE, "SetWindowSubclass failed");
#endif
}

void DisplayWindow::removeSubclass() noexcept {
#ifdef _WIN32
    if (window == nullptr) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) return;
    if (info.subsystem != SDL_SYSWM_WINDOWS) return;
    RemoveWindowSubclass(info.info.win.window, reinterpret_cast<SUBCLASSPROC>(&DisplayWindow::windowProc), DisplayWindowSubclassId);
#endif
}

void DisplayWindow::applyAspectRatio(void* hwnd, std::uintptr_t edge, void* rect) const {
#ifdef _WIN32
    require(aspectWidth != 0 && aspectHeight != 0, "source extent must be non-zero during resize");
    RECT windowBounds{};
    RECT clientBounds{};
    require(GetWindowRect(static_cast<HWND>(hwnd), &windowBounds) != FALSE, "GetWindowRect failed");
    require(GetClientRect(static_cast<HWND>(hwnd), &clientBounds) != FALSE, "GetClientRect failed");
    const auto frameWidth = (windowBounds.right - windowBounds.left) - (clientBounds.right - clientBounds.left);
    const auto frameHeight = (windowBounds.bottom - windowBounds.top) - (clientBounds.bottom - clientBounds.top);
    require(frameWidth >= 0 && frameHeight >= 0, "invalid window frame extent");
    auto* bounds = static_cast<RECT*>(rect);
    const auto clientWidth = bounds->right - bounds->left - frameWidth;
    const auto clientHeight = bounds->bottom - bounds->top - frameHeight;
    require(clientWidth > 0 && clientHeight > 0, "resized client extent must be positive");
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        const auto width = AgcDriver::ComputeWidthForHeight_nid_postfix(aspectWidth, aspectHeight, static_cast<std::uint32_t>(clientHeight));
        bounds->right = bounds->left + static_cast<LONG>(width) + frameWidth;
        return;
    }
    const auto height = AgcDriver::ComputeHeightForWidth_nid_postfix(aspectWidth, aspectHeight, static_cast<std::uint32_t>(clientWidth));
    if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) bounds->top = bounds->bottom - static_cast<LONG>(height) - frameHeight;
    else bounds->bottom = bounds->top + static_cast<LONG>(height) + frameHeight;
#else
    static_cast<void>(hwnd);
    static_cast<void>(edge);
    static_cast<void>(rect);
#endif
}

std::intptr_t DisplayWindow::windowProc(void* hwnd, unsigned int message, std::uintptr_t wParam, std::intptr_t lParam, std::uintptr_t subclassId, std::uintptr_t referenceData) {
#ifdef _WIN32
    static_cast<void>(subclassId);
    if (message == WM_SIZING) {
        reinterpret_cast<const DisplayWindow*>(referenceData)->applyAspectRatio(hwnd, wParam, reinterpret_cast<void*>(lParam));
        return TRUE;
    }
    return DefSubclassProc(static_cast<HWND>(hwnd), message, static_cast<WPARAM>(wParam), static_cast<LPARAM>(lParam));
#else
    static_cast<void>(hwnd);
    static_cast<void>(message);
    static_cast<void>(wParam);
    static_cast<void>(lParam);
    static_cast<void>(subclassId);
    static_cast<void>(referenceData);
    return 0;
#endif
}
