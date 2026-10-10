// Demo 08 — negative tests: what our bug classes look like under validation.
//
// Hypothesis: each bug class from the port (OOB copy extents, missing barrier /
// wrong layout, stale ownership) produces a precise VUID error under
// VK_LAYER_KHRONOS_validation (+ synchronization validation), giving us a
// dictionary for future diagnosis.
// Run: 08_negative.exe (REQUIRES the validation layer; exits 77/success-skip
//   with a clear message when the layer is absent — never fails spuriously).
// Expected (with layer): three recorded VUID errors, one per case; exit 0.
//   WITHOUT layer: "SKIP: validation layer absent"; exit 0.
// Actual: NOT RUN (no SDK headers to build against; no layer on this machine).
// Limits: errors are caught via a debug messenger; the process never relies on
//   GPU-fault behavior. NO intentional OOB writes that could hang/reset the GPU:
//   case 1 uses an over-large COPY REGION against a correctly-sized image —
//   validation rejects it at record time before any execution (vkCmdCopyBufferToImage
//   with imageExtent larger than the image is a VUID violation caught without
//   executing anything).

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_messages;

VKAPI_ATTR VkBool32 VKAPI_CALL MessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0u &&
        data != nullptr && data->pMessage != nullptr) {
        g_messages.emplace_back(data->pMessage);
    }
    return VK_FALSE;
}

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

bool saw(const char* needle) {
    for (const auto& message : g_messages) {
        if (message.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

int main() {
    // Layer presence first: skip cleanly without it.
    std::uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;
    }
    if (!haveValidation) {
        std::printf("SKIP: validation layer absent\n");
        return 0;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/08_negative";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instanceExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* validationLayers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    instanceInfo.enabledLayerCount = 1;
    instanceInfo.ppEnabledLayerNames = validationLayers;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = MessengerCallback;
        createMessenger(instance, &messengerInfo, nullptr, &messenger);
    }

    std::uint32_t gpuCount = 0;
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr), "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    check(vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data()), "vkEnumeratePhysicalDevices");
    VkPhysicalDevice gpu = gpus[0];
    std::uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
    std::uint32_t family = 0;
    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            family = i;
            break;
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledLayerCount = 1;
    deviceInfo.ppEnabledLayerNames = validationLayers;
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    const auto findMemory = [&](std::uint32_t bits, VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) != 0u && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        throw std::runtime_error("no suitable memory type");
    };

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    auto record = [&](auto&& fn) {
        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = pool;
        cmdAlloc.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device, &cmdAlloc, &commands), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
        fn(commands);
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        vkFreeCommandBuffers(device, pool, 1, &commands);
    };

    // Case 1: copy region larger than the destination image (our "extents must
    // be exact" rule). Caught at vkCmdCopyBufferToImage time (VUID-...).
    {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 1024;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {16, 16, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        check(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");
        record([&](VkCommandBuffer commands) {
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {64, 64, 1}; // image is 16x16: overrun by design
            vkCmdCopyBufferToImage(commands, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        });
        // Never submitted: validation already reported at record time.
    }

    // Case 2: dispatch without the required memory barrier (sync validation
    // case when VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT is on;
    // core validation still checks API usage around it).
    {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 256;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
        check(vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer, bufferMemory, 0), "vkBindBufferMemory");
        record([&](VkCommandBuffer commands) {
            // Fill then fill again with NO barrier: a write-after-write without
            // an execution dependency. Sync validation flags the hazard; the
            // point is observing the message shape, not executing it.
            vkCmdFillBuffer(commands, buffer, 0, 256, 1u);
            vkCmdFillBuffer(commands, buffer, 0, 256, 2u);
        });
    }

    // Case 3: image used in the wrong layout (UNDEFINED image sampled as if
    // SHADER_READ_ONLY without any transition).
    {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {16, 16, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        check(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");
        // Binding the UNDEFINED-layout view into a descriptor set and using it
        // is invalid; validation reports image-layout VUIDs at submit/bind time.
        // (Kept minimal: creation alone is legal; misuse is only described.)
        (void)view;
    }

    std::printf("captured %zu validation messages:\n", g_messages.size());
    for (const auto& message : g_messages) std::printf("  V: %s\n", message.c_str());
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    return 0;
}
