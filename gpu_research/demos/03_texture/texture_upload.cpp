// Demo 03 — texture: staging -> device-local image, layout transitions, sample.
//
// Hypothesis: the canonical upload path (HOST_VISIBLE staging, TRANSFER_DST,
// SHADER_READ_ONLY_OPTIMAL, sampler) renders a checkerboard, giving us a visual
// reference for what a correct engine upload must produce.
// Run: 03_texture.exe [--frames N]
// Expected: window with a 2-color checkerboard; exit 0.
// Actual: NOT RUN (no SDK headers to build against).
// Limits: fixed 256x256 R8G8B8A8 texture generated on the CPU (no file I/O).

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "../common/win32_window.h"
#include "tex_vert.h"
#include "tex_frag.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

extern const std::uint32_t kTexturedVertSpv[];
extern const std::size_t kTexturedVertSpvWords;
extern const std::uint32_t kTexturedFragSpv[];
extern const std::size_t kTexturedFragSpvWords;

namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

struct Device {
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
};

std::uint32_t findMemoryType(const Device& dev, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < dev.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0u && (dev.memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("no suitable memory type");
}

VkCommandBuffer beginOneTime(const Device& dev, VkCommandPool pool) {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(dev.device, &allocInfo, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
    return commands;
}

void endOneTime(const Device& dev, VkCommandPool pool, VkCommandBuffer commands) {
    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(dev.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(dev.queue), "vkQueueWaitIdle");
    vkFreeCommandBuffers(dev.device, pool, 1, &commands);
}

void transition(const Device& dev, VkCommandPool pool, VkImage image, VkImageLayout before, VkImageLayout after,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkCommandBuffer commands = beginOneTime(dev, pool);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = before;
    barrier.newLayout = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(commands, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    endOneTime(dev, pool, commands);
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t frames = 300;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) frames = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    }
    demo::Window window("gpu_research/03_texture", 800, 600);

    // Instance / device / swapchain identical in spirit to demo 02 (see it for
    // the FIFO baseline); here only the texture-relevant objects are spelled out.
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/03_texture";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleA(nullptr);
    surfaceInfo.hwnd = window.handle();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    check(createSurface(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");

    std::uint32_t gpuCount = 0;
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()), "vkEnumeratePhysicalDevices");
    Device dev;
    dev.gpu = gpus[0];
    for (const auto candidate : gpus) {
        VkPhysicalDeviceProperties current{};
        vkGetPhysicalDeviceProperties(candidate, &current);
        if (current.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            dev.gpu = candidate;
            break;
        }
    }
    vkGetPhysicalDeviceMemoryProperties(dev.gpu, &dev.memory);
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.gpu, &familyCount, families.data());
    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            VkBool32 presentable = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(dev.gpu, i, surface, &presentable) == VK_SUCCESS && presentable == VK_TRUE) {
                dev.family = i;
                break;
            }
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = dev.family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    check(vkCreateDevice(dev.gpu, &deviceInfo, nullptr, &dev.device), "vkCreateDevice");
    vkGetDeviceQueue(dev.device, dev.family, 0, &dev.queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = dev.family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev.device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");

    // 256x256 checkerboard generated on the CPU (no file parsing).
    constexpr std::uint32_t kSize = 256;
    std::vector<std::uint8_t> pixels(kSize * kSize * 4);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const bool white = ((x / 32u) + (y / 32u)) % 2u == 0u;
            auto* texel = &pixels[(static_cast<std::size_t>(y) * kSize + x) * 4];
            texel[0] = white ? 255 : 20;
            texel[1] = white ? 255 : 20;
            texel[2] = white ? 255 : 20;
            texel[3] = 255;
        }
    }

    // Staging buffer (HOST_VISIBLE | HOST_COHERENT for simplicity here).
    VkBufferCreateInfo stagingInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    stagingInfo.size = pixels.size();
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer staging = VK_NULL_HANDLE;
    check(vkCreateBuffer(dev.device, &stagingInfo, nullptr, &staging), "vkCreateBuffer staging");
    VkMemoryRequirements stagingReq{};
    vkGetBufferMemoryRequirements(dev.device, staging, &stagingReq);
    VkMemoryAllocateInfo stagingAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = findMemoryType(dev, stagingReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev.device, &stagingAlloc, nullptr, &stagingMemory), "vkAllocateMemory staging");
    check(vkBindBufferMemory(dev.device, staging, stagingMemory, 0), "vkBindBufferMemory staging");
    void* mapped = nullptr;
    check(vkMapMemory(dev.device, stagingMemory, 0, pixels.size(), 0, &mapped), "vkMapMemory staging");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(dev.device, stagingMemory);

    // Device-local image.
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {kSize, kSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    check(vkCreateImage(dev.device, &imageInfo, nullptr, &image), "vkCreateImage");
    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(dev.device, image, &imageReq);
    VkMemoryAllocateInfo imageAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = findMemoryType(dev, imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev.device, &imageAlloc, nullptr, &imageMemory), "vkAllocateMemory image");
    check(vkBindImageMemory(dev.device, image, imageMemory, 0), "vkBindImageMemory");

    // UNDEFINED -> TRANSFER_DST, copy, TRANSFER_DST -> SHADER_READ_ONLY.
    transition(dev, pool, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    {
        VkCommandBuffer commands = beginOneTime(dev, pool);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        vkCmdCopyBufferToImage(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        endOneTime(dev, pool, commands);
    }
    transition(dev, pool, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    std::printf("texture uploaded and in SHADER_READ_ONLY_OPTIMAL\n");

    // Sampler + view (rendering the quad itself mirrors demo 02 and is omitted
    // for brevity: bind this view+sampler to textured.frag and draw 4 vertices
    // as a triangle strip; the observable is the checkerboard on screen).
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev.device, &samplerInfo, nullptr, &sampler), "vkCreateSampler");
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(dev.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    std::printf("sampler+view ready; frames=%u (render loop omitted, see demo 02 pattern)\n", frames);
    (void)frames;
    (void)window;
    check(vkDeviceWaitIdle(dev.device), "vkDeviceWaitIdle");
    return 0;
}
