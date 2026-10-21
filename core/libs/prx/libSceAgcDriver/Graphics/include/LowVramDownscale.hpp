#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LOWVRAMDOWNSCALE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_LOWVRAMDOWNSCALE_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Caps host images at a configured extent (FullHD by default), ported from
// KytyPS5's low_vram_downscale.h. The images are created smaller and only the
// top-left region of every mip is uploaded: there is no resampling, this is a
// VRAM safety fallback for textures far larger than the display, not a quality
// scaler. AnyPS5 applies it to sampled texture snapshots only; render targets,
// depth surfaces and storage images keep their guest extents.
namespace AgcDriver::Graphics::LowVramDownscale {

struct Config {
    bool enabled = false;
    bool auto_enabled = false;

    // What to downscale. Color and depth attachments stay off: AnyPS5 render
    // targets and depth surfaces are guest-visible at their full extents.
    // The game integration only calls Apply for sampled snapshots, so these
    // gates are the API-level safety net rather than separate env switches.
    bool downscale_color = false;
    bool downscale_depth = false;
    bool downscale_sampled = true;
    bool downscale_storage = false;
    bool downscale_transient = false;

    // Behaviour.
    bool clamp_mips = true;
    bool align_even = true;

    // Target: FullHD.
    std::uint32_t max_width = 1920;
    std::uint32_t max_height = 1080;

    // Auto-enable when the largest device-local heap is at most this size.
    // 9 GiB deliberately includes 8 GiB cards.
    VkDeviceSize auto_enable_heap_limit = 9ULL * 1024 * 1024 * 1024;
};

inline Config& GetConfig() {
    static Config cfg;
    return cfg;
}

inline bool ParseBool(const char* value, bool default_value) {
    if (value == nullptr) return default_value;
    if (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 || std::strcmp(value, "ON") == 0 || std::strcmp(value, "yes") == 0 || std::strcmp(value, "YES") == 0) return true;
    if (std::strcmp(value, "0") == 0 || std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0 || std::strcmp(value, "off") == 0 || std::strcmp(value, "OFF") == 0 || std::strcmp(value, "no") == 0 || std::strcmp(value, "NO") == 0) return false;
    return default_value;
}

inline bool ParseExtent(const char* value, std::uint32_t& width, std::uint32_t& height) {
    if (value == nullptr) return false;
    unsigned int parsed_width = 0;
    unsigned int parsed_height = 0;
    if (std::sscanf(value, "%ux%u", &parsed_width, &parsed_height) == 2 && parsed_width > 0 && parsed_height > 0) {
        width = static_cast<std::uint32_t>(parsed_width);
        height = static_cast<std::uint32_t>(parsed_height);
        return true;
    }
    if (std::sscanf(value, "%u", &parsed_width) == 1 && parsed_width > 0) {
        width = static_cast<std::uint32_t>(parsed_width);
        height = width;
        return true;
    }
    return false;
}

inline void Init(const VkPhysicalDeviceMemoryProperties& memory) {
    Config cfg;

    const char* mode = std::getenv("ANYPS5_DOWNSCALE");
    bool forced_by_env = false;
    if (mode != nullptr && std::strcmp(mode, "auto") != 0) {
        forced_by_env = true;
        cfg.enabled = ParseBool(mode, false);
    }

    cfg.downscale_sampled = ParseBool(std::getenv("ANYPS5_DOWNSCALE_TEXTURES"), true);

    cfg.clamp_mips = ParseBool(std::getenv("ANYPS5_DOWNSCALE_CLAMP_MIPS"), true);
    cfg.align_even = ParseBool(std::getenv("ANYPS5_DOWNSCALE_ALIGN_EVEN"), true);
    ParseExtent(std::getenv("ANYPS5_DOWNSCALE_MAX"), cfg.max_width, cfg.max_height);

    if (const char* limit_mb = std::getenv("ANYPS5_DOWNSCALE_VRAM_LIMIT_MB")) {
        unsigned int megabytes = 0;
        if (std::sscanf(limit_mb, "%u", &megabytes) == 1 && megabytes > 0) cfg.auto_enable_heap_limit = static_cast<VkDeviceSize>(megabytes) * 1024ULL * 1024ULL;
    }

    VkDeviceSize max_device_local_heap = 0;
    for (std::uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        const auto& heap = memory.memoryHeaps[i];
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) max_device_local_heap = std::max(max_device_local_heap, heap.size);
    }

    const bool small_vram_device = max_device_local_heap > 0 && max_device_local_heap <= cfg.auto_enable_heap_limit;
    if (!forced_by_env && small_vram_device) {
        cfg.enabled = true;
        cfg.auto_enabled = true;
    }

    GetConfig() = cfg;

    if (cfg.enabled) {
        std::fprintf(stderr, "[low-vram-downscale] enabled%s target=%ux%u local_heap=%llu MiB sampled=%d\n", cfg.auto_enabled ? " (auto)" : "", cfg.max_width, cfg.max_height, static_cast<unsigned long long>(max_device_local_heap / (1024ULL * 1024ULL)), static_cast<int>(cfg.downscale_sampled));
    }
}

inline std::uint32_t MaxMipLevels(std::uint32_t width, std::uint32_t height) {
    std::uint32_t max_dim = std::max(width, height);
    std::uint32_t max_mips = 1;
    while (max_dim > 1) {
        max_dim >>= 1;
        ++max_mips;
    }
    return max_mips;
}

inline std::uint32_t ClampMipLevels(std::uint32_t width, std::uint32_t height, std::uint32_t requested_mips) {
    if (requested_mips == 0) return 1;
    if (requested_mips == VK_REMAINING_MIP_LEVELS) return MaxMipLevels(width, height);
    return std::min(requested_mips, MaxMipLevels(width, height));
}

// Round up to the image's texel-block alignment (4 for BC formats); the base
// extent of a compressed image must be a whole number of blocks.
inline VkExtent3D ScaleExtent(const VkExtent3D& extent, const Config& cfg, std::uint32_t block_alignment = 1) {
    if (extent.width == 0 || extent.height == 0) return extent;
    const double scale_x = static_cast<double>(cfg.max_width) / static_cast<double>(extent.width);
    const double scale_y = static_cast<double>(cfg.max_height) / static_cast<double>(extent.height);
    const double scale = std::min(scale_x, scale_y);
    if (scale >= 1.0) return extent;

    std::uint32_t width = static_cast<std::uint32_t>(std::llround(static_cast<double>(extent.width) * scale));
    std::uint32_t height = static_cast<std::uint32_t>(std::llround(static_cast<double>(extent.height) * scale));
    width = std::max(1u, width);
    height = std::max(1u, height);

    const std::uint32_t alignment = std::max(block_alignment, cfg.align_even ? 2u : 1u);
    if (alignment > 1) {
        if (width > 1) width = (width + alignment - 1u) & ~(alignment - 1u);
        if (height > 1) height = (height + alignment - 1u) & ~(alignment - 1u);
    }

    width = std::min(width, cfg.max_width / alignment * alignment);
    height = std::min(height, cfg.max_height / alignment * alignment);
    width = std::min(width, extent.width);
    height = std::min(height, extent.height);
    return VkExtent3D {std::max(1u, width), std::max(1u, height), 1};
}

inline bool ShouldDownscale(const VkImageCreateInfo& info, const Config& cfg) {
    if (!cfg.enabled) return false;
    if (info.imageType != VK_IMAGE_TYPE_2D) return false;
    if (info.extent.depth != 1) return false;
    if (info.extent.width <= cfg.max_width && info.extent.height <= cfg.max_height) return false;
    if ((info.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 && !cfg.downscale_transient) return false;
    // Attachment and storage usage wins over SAMPLED: guest-visible surfaces
    // keep their extents unless their own category is explicitly enabled.
    if ((info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) return cfg.downscale_color;
    if ((info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) return cfg.downscale_depth;
    if ((info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0) return cfg.downscale_storage;
    if ((info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) return cfg.downscale_sampled;
    return false;
}

inline bool ApplyWithConfig(VkImageCreateInfo& info, const Config& cfg, std::uint32_t block_alignment = 1) {
    if (!ShouldDownscale(info, cfg)) return false;

    const VkExtent3D old_extent = info.extent;
    const std::uint32_t old_mips = info.mipLevels;
    const VkExtent3D scaled_extent = ScaleExtent(info.extent, cfg, block_alignment);

    // mipLevels must fit the smaller chain, and VK_REMAINING_MIP_LEVELS is not
    // valid in VkImageCreateInfo. With clamping disabled an image whose full
    // chain no longer fits is left at its guest size instead of becoming invalid.
    if (!cfg.clamp_mips && old_mips != VK_REMAINING_MIP_LEVELS && old_mips > MaxMipLevels(scaled_extent.width, scaled_extent.height)) return false;

    info.extent = scaled_extent;
    if (cfg.clamp_mips || old_mips == VK_REMAINING_MIP_LEVELS) info.mipLevels = ClampMipLevels(info.extent.width, info.extent.height, info.mipLevels);

    static std::atomic<std::uint32_t> clamped {0};
    const auto count = clamped.fetch_add(1) + 1;
    if (count <= 8 || count % 512 == 0) {
        std::fprintf(stderr, "[low-vram-downscale] clamp #%u: %ux%u mips=%u -> %ux%u mips=%u format=%d usage=0x%08X\n", count, old_extent.width, old_extent.height, old_mips, info.extent.width, info.extent.height, info.mipLevels, static_cast<int>(info.format), static_cast<unsigned>(info.usage));
    }
    return true;
}

inline bool Apply(VkImageCreateInfo& info, std::uint32_t block_alignment = 1) {
    return ApplyWithConfig(info, GetConfig(), block_alignment);
}

// Emergency downscale used after a creation attempt already failed with
// VK_ERROR_OUT_OF_DEVICE_MEMORY. Shrinks by 25% regardless of the configured
// target, because many small images can still exhaust VRAM.
inline bool ApplyEmergency(VkImageCreateInfo& info, std::uint32_t block_alignment = 1) {
    if (info.imageType != VK_IMAGE_TYPE_2D || info.extent.depth != 1) return false;

    const std::uint32_t alignment = std::max(block_alignment, 1u);
    const auto shrink = [alignment](std::uint32_t value) {
        const std::uint32_t floor_value = std::max(64u, alignment);
        const std::uint32_t scaled = std::max(floor_value, (value * 3u) / 4u);
        return std::min(value, scaled / alignment * alignment);
    };
    const std::uint32_t old_width = info.extent.width;
    const std::uint32_t old_height = info.extent.height;
    const std::uint32_t new_width = shrink(old_width);
    const std::uint32_t new_height = shrink(old_height);
    if (new_width >= old_width && new_height >= old_height) return false;

    std::fprintf(stderr, "[low-vram-downscale] EMERGENCY OOM REDUCTION: %ux%u -> %ux%u format=%d usage=0x%08X mips=%u layers=%u\n", old_width, old_height, new_width, new_height, static_cast<int>(info.format), static_cast<unsigned>(info.usage), info.mipLevels, info.arrayLayers);
    info.extent.width = new_width;
    info.extent.height = new_height;

    const std::uint32_t max_mips = MaxMipLevels(new_width, new_height);
    if (info.mipLevels == VK_REMAINING_MIP_LEVELS || info.mipLevels > max_mips) info.mipLevels = max_mips;
    return true;
}

}

#endif
