// Demo 01 — device, queues and capabilities check.
//
// Hypothesis: the RTX 3050 exposes every feature our engine requires, so no
// capability fallback can explain rendering failures on this machine.
// Source: Vulkan Guide (querying extensions/features), vulkaninfo cross-check.
// Run: 01_device.exe [--expect-all]  (no window, no SDK runtime deps beyond the loader)
// Expected: prints instance 1.x, device name/drivers, per-feature true/false,
//   memory heaps/types; exit 0. With --expect-all exits 3 when any required
//   feature is missing.
// Actual: NOT RUN (no SDK headers to build against).
// Limits: reports capabilities only; does not test behavior. Validation layer
//   presence is reported, never required (graceful skip, as on this machine).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* name, bool required) {
    std::printf("  %-38s : %s%s\n", name, ok ? "true" : "false", (required && !ok) ? "  <-- REQUIRED, MISSING" : "");
    if (required && !ok) ++g_failures;
}

} // namespace

int main(int argc, char** argv) {
    const bool expectAll = argc > 1 && std::strcmp(argv[1], "--expect-all") == 0;

    // 1. Loader + instance layers/extensions visible without any SDK.
    std::uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    std::printf("instance layers (%u):\n", layerCount);
    for (const auto& layer : layers) {
        std::printf("  %s (v%u)\n", layer.layerName, layer.specVersion);
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;
    }
    std::printf("validation layer: %s\n", haveValidation ? "present" : "ABSENT");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/01_device";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateInstance failed\n");
        return 2;
    }

    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::printf("physical devices: %u\n", deviceCount);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto gpu : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(gpu, &props);
        std::printf("device: %s (id %04x:%04x, driver 0x%x, api %u.%u.%u)\n", props.deviceName, props.vendorID,
                    props.deviceID, props.driverVersion, VK_VERSION_MAJOR(props.apiVersion),
                    VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

        // Queues.
        std::uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            std::printf("  queue family %u: count %u flags 0x%x\n", i, families[i].queueCount, families[i].queueFlags);
        }

        // Core 1.1/1.2 features our engine requires.
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceRobustness2FeaturesKHR robust2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
        v12.pNext = &robust2;
        features.pNext = &v12;
        vkGetPhysicalDeviceFeatures2(gpu, &features);
        std::printf("features:\n");
        check(features.features.shaderInt64 == VK_TRUE, "shaderInt64", true);
        check(features.features.robustBufferAccess == VK_TRUE, "robustBufferAccess", false);
        check(v12.bufferDeviceAddress == VK_TRUE, "bufferDeviceAddress", true);
        check(v12.descriptorIndexing == VK_TRUE, "descriptorIndexing", true);
        check(v12.timelineSemaphore == VK_TRUE, "timelineSemaphore", false);
        check(robust2.robustBufferAccess2 == VK_TRUE, "robustBufferAccess2", false);
        check(robust2.robustImageAccess2 == VK_TRUE, "robustImageAccess2", false);

        // A few promoted/extension features behind their own structs.
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        mesh.pNext = &bary;
        bary.pNext = &interlock;
        VkPhysicalDeviceFeatures2 ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        ext.pNext = &mesh;
        vkGetPhysicalDeviceFeatures2(gpu, &ext);
        check(mesh.meshShader == VK_TRUE, "meshShader (EXT)", false);
        check(bary.fragmentShaderBarycentric == VK_TRUE, "fragmentShaderBarycentric", false);
        check(interlock.fragmentShaderPixelInterlock == VK_TRUE, "fragmentShaderPixelInterlock", false);

        // Memory model.
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
        std::printf("  heaps=%u types=%u\n", memory.memoryHeapCount, memory.memoryTypeCount);
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            std::printf("  type %u: heap %u flags 0x%x\n", i, memory.memoryTypes[i].heapIndex, memory.memoryTypes[i].propertyFlags);
        }
    }

    vkDestroyInstance(instance, nullptr);
    if (expectAll && g_failures != 0) {
        std::fprintf(stderr, "missing %d required features\n", g_failures);
        return 3;
    }
    std::printf("done\n");
    return 0;
}
