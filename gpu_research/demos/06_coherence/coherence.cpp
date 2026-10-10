// Demo 06 — CPU/GPU coherence: flush/invalidate around a shared pattern buffer.
//
// Hypothesis: with a NON-COHERENT host-visible allocation, explicit
// vkFlushMappedMemoryRanges (CPU write -> GPU read) and
// vkInvalidateMappedMemoryRanges (GPU write -> CPU read) are necessary and
// sufficient for visibility; without them the round-trip may read stale data.
// Run: 06_coherence.exe
// Expected: "coherent=N flush/invalidate round-trip OK"; exit 0. When no
//   non-coherent type exists, runs the coherent path and reports it (weaker).
// Actual: NOT RUN (no SDK headers to build against).
// Limits: one compute dispatch writing a pattern; no import extensions involved
//   (plain mapped memory — the import case adds ownership transfer on top).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "bda_comp.h"

extern const std::uint32_t kBdaWriteSpv[];
extern const std::size_t kBdaWriteSpvWords;

namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

constexpr std::uint32_t kDwords = 4096;
constexpr std::uint32_t kPattern = 0x5EED1234u;

} // namespace

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/06_coherence";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
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
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u) {
            family = i;
            break;
        }
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.features.shaderInt64 = VK_TRUE;
    enabled.pNext = &enabled12;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pNext = &enabled;
    deviceInfo.pEnabledFeatures = &enabled.features;
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);

    // Prefer a NON-COHERENT host-visible type to prove flush/invalidate matter.
    std::uint32_t typeIndex = 0;
    bool coherent = true;
    bool picked = false;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount && !picked; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u) continue;
        if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u) {
            typeIndex = i;
            coherent = false;
            picked = true;
        }
    }
    if (!picked) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((memory.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                typeIndex = i;
                picked = true;
                break;
            }
        }
    }
    if (!picked) throw std::runtime_error("no host-visible memory type");
    std::printf("memory type %u (%s)\n", typeIndex, coherent ? "coherent" : "NON-COHERENT");

    constexpr VkDeviceSize kBytes = static_cast<VkDeviceSize>(kDwords) * 4u;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = kBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    if ((req.memoryTypeBits & (1u << typeIndex)) == 0u) throw std::runtime_error("buffer cannot use the chosen type");
    VkMemoryAllocateFlagsInfo flagsInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    allocInfo.pNext = &flagsInfo;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    check(vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, buffer, bufferMemory, 0), "vkBindBufferMemory");
    auto getAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(device, "vkGetBufferDeviceAddress"));
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = buffer;
    const VkDeviceAddress address = getAddress(device, &addressInfo);

    // CPU write of a seed value, then FLUSH so the GPU must see it.
    void* mapped = nullptr;
    check(vkMapMemory(device, bufferMemory, 0, kBytes, 0, &mapped), "vkMapMemory");
    static_cast<std::uint32_t*>(mapped)[0] = 0xDEADBEEF;
    if (!coherent) {
        VkMappedMemoryRange flush{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        flush.memory = bufferMemory;
        flush.offset = 0;
        flush.size = VK_WHOLE_SIZE;
        check(vkFlushMappedMemoryRanges(device, 1, &flush), "vkFlushMappedMemoryRanges");
    }

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = kBdaWriteSpvWords * sizeof(std::uint32_t);
    moduleInfo.pCode = kBdaWriteSpv;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = 16;
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "vkCreateComputePipelines");
    vkDestroyShaderModule(device, module, nullptr);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdAlloc.commandPool = pool;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &cmdAlloc, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    struct Push {
        std::uint64_t address;
        std::uint32_t pattern;
        std::uint32_t pad = 0;
    } push{address, kPattern, 0};
    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(commands, kDwords, 1, 1);
    VkBufferMemoryBarrier done{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    done.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    done.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    done.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    done.buffer = buffer;
    done.offset = 0;
    done.size = kBytes;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &done, 0, nullptr);
    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    // GPU wrote: INVALIDATE before the CPU reads (required when non-coherent).
    if (!coherent) {
        VkMappedMemoryRange invalidate{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        invalidate.memory = bufferMemory;
        invalidate.offset = 0;
        invalidate.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device, 1, &invalidate), "vkInvalidateMappedMemoryRanges");
    }
    const auto* words = static_cast<const std::uint32_t*>(mapped);
    bool ok = true;
    for (std::uint32_t i = 0; i < kDwords; ++i) {
        if (words[i] != kPattern) {
            std::fprintf(stderr, "mismatch at dword %u: 0x%x\n", i, words[i]);
            ok = false;
            break;
        }
    }
    vkUnmapMemory(device, bufferMemory);
    std::printf("coherent=%d flush/invalidate round-trip %s\n", coherent ? 1 : 0, ok ? "OK" : "MISMATCH");
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    return ok ? 0 : 1;
}
