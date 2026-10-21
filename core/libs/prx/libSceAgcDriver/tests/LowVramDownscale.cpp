#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/LowVramDownscale.hpp"
#include <cstdint>

namespace {

using namespace AgcDriver::Graphics;
using namespace AgcDriver::Graphics::LowVramDownscale;

VkImageCreateInfo SampledImage(std::uint32_t width, std::uint32_t height, std::uint32_t mips, VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VkImageType type = VK_IMAGE_TYPE_2D) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = type;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1u};
    info.mipLevels = mips;
    info.arrayLayers = 1;
    info.usage = usage;
    return info;
}

}

void RunLowVramDownscaleTests() {
    Require(ParseBool("1", false) && ParseBool("on", false) && !ParseBool("0", true) && !ParseBool("off", true), "boolean env parsing is incorrect");
    Require(ParseBool(nullptr, true) && ParseBool("garbage", true), "boolean env parsing did not fall back to its default");
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Require(ParseExtent("1280x720", width, height) && width == 1280 && height == 720, "WxH env parsing is incorrect");
    Require(ParseExtent("1280", width, height) && width == 1280 && height == 1280, "square env parsing is incorrect");
    Require(!ParseExtent("0x0", width, height) && !ParseExtent("nope", width, height) && !ParseExtent(nullptr, width, height), "invalid extents were accepted");

    Config disabled;
    auto info = SampledImage(3840, 2160, 13);
    Require(!ApplyWithConfig(info, disabled), "disabled downscale changed an image");
    Require(info.extent.width == 3840 && info.extent.height == 2160 && info.mipLevels == 13, "disabled downscale changed the extent");

    Config config;
    config.enabled = true;
    info = SampledImage(3840, 2160, 13);
    Require(ApplyWithConfig(info, config), "4K sampled texture was not downscaled");
    Require(info.extent.width == 1920 && info.extent.height == 1080, "4K sampled texture was not capped at FullHD");
    Require(info.mipLevels == 11, "downscaled mip chain was not clamped");
    Require(info.arrayLayers == 1 && info.format == VK_FORMAT_R8G8B8A8_UNORM && info.usage == (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT), "downscale changed unrelated image fields");

    info = SampledImage(3840, 2160, VK_REMAINING_MIP_LEVELS);
    Require(ApplyWithConfig(info, config) && info.mipLevels == 11, "remaining mip levels stayed unresolved in image create info");

    info = SampledImage(1920, 1080, 11);
    Require(!ApplyWithConfig(info, config), "FullHD sampled texture was downscaled");

    info = SampledImage(4096, 2160, 13);
    Require(ApplyWithConfig(info, config, 4), "block-compressed sampled texture was not downscaled");
    Require(info.extent.width == 1920 && info.extent.height == 1016 && info.extent.height % 4 == 0, "block-compressed extent is not block aligned");

    info = SampledImage(3840, 2160, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    Require(!ApplyWithConfig(info, config), "storage-only image was downscaled");

    info = SampledImage(3840, 2160, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    Require(!ApplyWithConfig(info, config), "color attachment was downscaled by default");
    config.downscale_color = true;
    Require(ApplyWithConfig(info, config), "color attachment downscale switch was ignored");
    config.downscale_color = false;

    info = SampledImage(3840, 2160, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    Require(!ApplyWithConfig(info, config), "sampled render target was downscaled by default");

    info = SampledImage(3840, 2160, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TYPE_3D);
    Require(!ApplyWithConfig(info, config), "3D image was downscaled");
    info = SampledImage(3840, 2160, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TYPE_1D);
    Require(!ApplyWithConfig(info, config), "1D image was downscaled");
    info = SampledImage(3840, 2160, 1);
    info.extent.depth = 2;
    Require(!ApplyWithConfig(info, config), "layered depth image was downscaled");

    config.align_even = false;
    config.max_width = 1001;
    config.max_height = 1001;
    info = SampledImage(2003, 2003, 1);
    Require(ApplyWithConfig(info, config), "odd extent was not downscaled");
    Require(info.extent.width == 1001 && info.extent.height == 1001, "alignment switch changed the scaled extent");
    config.align_even = true;
    config.max_width = 1920;
    config.max_height = 1080;

    config.clamp_mips = false;
    info = SampledImage(3840, 2160, 13);
    Require(!ApplyWithConfig(info, config), "full mip chain was downscaled with clamping disabled");
    Require(info.extent.width == 3840 && info.mipLevels == 13, "skipped downscale changed the image");
    info = SampledImage(3840, 2160, 1);
    Require(ApplyWithConfig(info, config) && info.extent.width == 1920, "single-mip image was not downscaled with clamping disabled");
    config.clamp_mips = true;

    Require(ClampMipLevels(1920, 1080, 13) == 11 && ClampMipLevels(1, 1, 13) == 1 && ClampMipLevels(64, 64, 0) == 1, "mip clamp is incorrect");
    Require(ClampMipLevels(64, 64, VK_REMAINING_MIP_LEVELS) == 7, "remaining mip levels were not resolved for image creation");

    info = SampledImage(3840, 2160, 13);
    Require(ApplyEmergency(info), "emergency reduction did not shrink a 4K image");
    Require(info.extent.width == 2880 && info.extent.height == 1620 && info.mipLevels == 12, "emergency reduction extent or mips are incorrect");
    info = SampledImage(100, 100, 1);
    Require(ApplyEmergency(info, 4), "emergency reduction did not shrink a small block-compressed image");
    Require(info.extent.width == 72 && info.extent.height == 72, "emergency block alignment is incorrect");
    info = SampledImage(10, 10, 1);
    Require(!ApplyEmergency(info), "emergency reduction shrank an image below its floor");
}
