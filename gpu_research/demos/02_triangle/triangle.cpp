// Demo 02 — triangle: window, swapchain (FIFO), render pass, pipeline, draw, present.
//
// Hypothesis: a minimal FIFO present loop renders stably on this machine, so any
// present-path instability in the port comes from engine code, not the driver.
// Run: 02_triangle.exe [--frames N]  (default 300; closes early on window close)
// Expected: window with an orange triangle for N frames; exit 0; with validation
//   layer present and VK_DIAG=1, zero validation errors.
// Actual: NOT RUN (no SDK headers to build against).
// Limits: fixed-function vertex input, one queue, no resize handling (recreating
//   the swapchain on resize is intentionally out of scope for this demo).

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "../common/win32_window.h"
#include "tri_vert.h"
#include "tri_frag.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// Precompiled SPIR-V blobs (compile shaders/triangle.vert|frag with
// glslangValidator -V at build time; CMake custom command, see CMakeLists).
extern const std::uint32_t kVertSpv[];
extern const std::size_t kVertSpvWords;
extern const std::uint32_t kFragSpv[];
extern const std::size_t kFragSpvWords;

namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

std::uint32_t findQueueFamily(VkPhysicalDevice gpu) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            return i;
    }
    throw std::runtime_error("no graphics+compute queue family");
}

std::uint32_t findMemoryType(VkPhysicalDeviceMemoryProperties* memory, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    for (std::uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0u && (memory->memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("no suitable memory type");
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t frames = 300;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0) frames = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    }
    demo::Window window("gpu_research/02_triangle", 800, 600);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/02_triangle";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 2;
    instanceInfo.ppEnabledExtensionNames = extensions;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = GetModuleHandleA(nullptr);
    surfaceInfo.hwnd = window.handle();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
    if (createSurface == nullptr) throw std::runtime_error("vkCreateWin32SurfaceKHR missing");
    check(createSurface(instance, &surfaceInfo, nullptr, &surface), "vkCreateWin32SurfaceKHR");

    std::uint32_t gpuCount = 0;
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr), "vkEnumeratePhysicalDevices");
    if (gpuCount == 0) throw std::runtime_error("no physical device");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()), "vkEnumeratePhysicalDevices");
    VkPhysicalDevice gpu = gpus[0]; // discrete preferred when several are present
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    for (const auto candidate : gpus) {
        VkPhysicalDeviceProperties current{};
        vkGetPhysicalDeviceProperties(candidate, &current);
        if (current.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            gpu = candidate;
            props = current;
            break;
        }
    }
    std::printf("device: %s\n", props.deviceName);
    const std::uint32_t family = findQueueFamily(gpu);

    VkBool32 presentable = VK_FALSE;
    check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface, &presentable), "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (presentable != VK_TRUE) throw std::runtime_error("queue cannot present");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    // Swapchain: FIFO baseline (our engine default), matching surface caps.
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    std::uint32_t formatCount = 0;
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formatCount, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
    VkSwapchainCreateInfoKHR swapInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapInfo.surface = surface;
    swapInfo.minImageCount = caps.minImageCount + 1 > caps.maxImageCount && caps.maxImageCount != 0 ? caps.maxImageCount : caps.minImageCount + 1;
    swapInfo.imageFormat = formats[0].format;
    swapInfo.imageColorSpace = formats[0].colorSpace;
    swapInfo.imageExtent = caps.currentExtent.width != 0xffffffffu ? caps.currentExtent : VkExtent2D{800, 600};
    swapInfo.imageArrayLayers = 1;
    swapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform = caps.currentTransform;
    swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // baseline under test
    swapInfo.clipped = VK_TRUE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    auto createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    check(createSwapchain(device, &swapInfo, nullptr, &swapchain), "vkCreateSwapchainKHR");
    std::printf("present mode: FIFO (requested and created)\n");

    std::uint32_t imageCount = 0;
    auto getImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(vkGetDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
    check(getImages(device, swapchain, &imageCount, nullptr), "vkGetSwapchainImagesKHR");
    std::vector<VkImage> images(imageCount);
    check(getImages(device, swapchain, &imageCount, images.data()), "vkGetSwapchainImagesKHR");

    // Render pass + pipeline (dynamic rendering would also do; render pass is explicit here).
    VkAttachmentDescription attachment{};
    attachment.format = swapInfo.imageFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    VkRenderPass pass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(device, &passInfo, nullptr, &pass), "vkCreateRenderPass");

    auto makeModule = [&](const std::uint32_t* words, std::size_t count) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = count * sizeof(std::uint32_t);
        moduleInfo.pCode = words;
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        return module;
    };
    const VkShaderModule vert = makeModule(kVertSpv, kVertSpvWords);
    const VkShaderModule frag = makeModule(kFragSpv, kFragSpvWords);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, static_cast<float>(swapInfo.imageExtent.width), static_cast<float>(swapInfo.imageExtent.height), 0, 1};
    VkRect2D scissor{{0, 0}, swapInfo.imageExtent};
    VkPipelineViewportStateCreateInfo viewports{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewports.viewportCount = 1;
    viewports.pViewports = &viewport;
    viewports.scissorCount = 1;
    viewports.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewports;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &samples;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.layout = layout;
    pipelineInfo.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);

    // Per-image views + framebuffers, one fence per frame in flight.
    std::vector<VkImageView> views(imageCount);
    std::vector<VkFramebuffer> framebuffers(imageCount);
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapInfo.imageFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device, &viewInfo, nullptr, &views[i]), "vkCreateImageView");
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &views[i];
        framebufferInfo.width = swapInfo.imageExtent.width;
        framebufferInfo.height = swapInfo.imageExtent.height;
        framebufferInfo.layers = 1;
        check(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]), "vkCreateFramebuffer");
    }
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
    auto acquireNext = reinterpret_cast<PFN_vkAcquireNextImageKHR>(vkGetDeviceProcAddr(device, "vkAcquireNextImageKHR"));
    auto queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(vkGetDeviceProcAddr(device, "vkQueuePresentKHR"));

    std::uint32_t presented = 0;
    while (presented < frames && window.pump()) {
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 1000000000ull), "vkWaitForFences");
        check(vkResetFences(device, 1, &fence), "vkResetFences");
        std::uint32_t index = 0;
        VkResult acquired = acquireNext(device, swapchain, 1000000000ull, VK_NULL_HANDLE, fence, &index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) throw std::runtime_error("vkAcquireNextImageKHR failed");
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &allocInfo, &commands), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
        const VkClearValue clear{{{1.0f, 0.5f, 0.0f, 1.0f}}};
        VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        passBegin.renderPass = pass;
        passBegin.framebuffer = framebuffers[index];
        passBegin.renderArea.extent = swapInfo.imageExtent;
        passBegin.clearValueCount = 1;
        passBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(commands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdDraw(commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(commands);
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        VkResult presentedResult = queuePresent(queue, &present);
        if (presentedResult != VK_SUCCESS && presentedResult != VK_SUBOPTIMAL_KHR) throw std::runtime_error("vkQueuePresentKHR failed");
        vkFreeCommandBuffers(device, pool, 1, &commands);
        ++presented;
    }
    std::printf("presented %u frames\n", presented);
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    return 0;
}
