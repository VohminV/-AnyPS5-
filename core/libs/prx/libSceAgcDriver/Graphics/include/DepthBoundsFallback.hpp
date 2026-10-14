#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHBOUNDSFALLBACK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_DEPTHBOUNDSFALLBACK_HPP

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace AgcDriver::Graphics {

// How depth-bounds test requests are served on the current device. Chosen
// once at device init by ResolveDepthBoundsBackend; pipelines and draws only
// read the outcome, never re-derive it.
enum class DepthBoundsBackend { Hardware, CpuFallback };

// Centralized mode selection: `supported` is
// VkPhysicalDeviceFeatures::depthBounds, `override` is APS5_DEPTH_BOUNDS
// (empty/auto, hardware, cpu; case-insensitive). Forced hardware without
// support, and unknown values, throw: a misconfigured or incapable setup
// must fail loudly, never silently disable the test.
DepthBoundsBackend ResolveDepthBoundsBackend(bool supported, std::string_view override);
const char* DepthBoundsBackendText(DepthBoundsBackend backend);

// Whether a depth-bounds request needs the CPU fallback: a depth plane with
// the test on while the fallback mode is selected (auto without the feature,
// or forced). The mode flag already encodes the hardware outcome, so no
// separate hardware parameter that could disagree with it. Single source for
// the decision (the pipeline also checks its stage list).
inline bool NeedsDepthBoundsFallback(bool hasDepth, bool testEnabled, bool fallbackMode) {
    return hasDepth && testEnabled && fallbackMode;
}

// Push-constant slot carrying [minDepthBounds, maxDepthBounds] for fallback
// pipelines (bytes and dwords). Fixed tail of the 128-byte block; a
// pipeline whose own push-constant range intersects it cannot use the
// fallback (honest error, never silent overlap).
inline constexpr std::uint32_t DepthBoundsPushOffsetBytes = 120;
inline constexpr std::uint32_t DepthBoundsPushMinDword = 30;
inline constexpr std::uint32_t DepthBoundsPushMaxDword = 31;

// Rewrites a fragment module to discard fragments outside the push-constant
// bounds (OpKill when FragCoord.z is outside [min, max], inclusive edges).
// The module keeps its single fragment entry point; hardware depth-bounds
// state must stay disabled for the rewritten module. Throws
// std::runtime_error naming the reason when the module cannot carry the
// fallback (not SPIR-V, entry count, depth-writing shader, unusable push
// layout, ...).
std::vector<std::uint32_t> ApplyDepthBoundsFallback(std::span<const std::uint32_t> spirv);

// Running totals for the [DepthBounds] diagnostics (fallback pipelines
// created, transforms applied, transform refusals). Updated at pipeline
// creation, never per frame.
struct DepthBoundsFallbackStats {
    std::uint64_t pipelines = 0;
    std::uint64_t transforms = 0;
    std::uint64_t refusals = 0;
    std::uint64_t transformMicroseconds = 0;
};
void NoteDepthBoundsFallbackPipeline(std::uint64_t transformMicroseconds);
void NoteDepthBoundsFallbackRefusal();
DepthBoundsFallbackStats GetDepthBoundsFallbackStats();

}

#endif
