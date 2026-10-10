// Demo 07 — queue sync: semaphore chain, fence wait, queue-family ownership.
//
// Hypothesis: work recorded on a transfer-capable path is visible to the graphics
// path only after an explicit semaphore+fence chain, and a buffer that changes
// queue families needs an explicit ownership transfer (release/acquire barrier).
// Run: 07_sync.exe
// Expected: "sync chain OK"; exit 0. Uses one queue family when only one exists
//   (ownership transfer then uses EXCLUSIVE + same family, still validated).
// Actual: NOT RUN (no SDK headers to build against).
// Limits: no swapchain/present; semaphore+ fence CPU wait (no timeline variant here).

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(what);
}

constexpr std::uint32_t kDwords = 1024;
constexpr std::uint32_t kPatternA = 0xAAAAAAAAu;
constexpr std::uint32_t kPatternB = 0xBBBBBBBBu;

} // namespace

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/07_sync";
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
    // Prefer two DIFFERENT families (transfer + graphics) to exercise a real
    // ownership transfer; fall back to one family otherwise.
    std::uint32_t transferFamily = 0xFFFFFFFFu, graphicsFamily = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0u && transferFamily == 0xFFFFFFFFu) transferFamily = i;
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u && graphicsFamily == 0xFFFFFFFFu) graphicsFamily = i;
    }
    if (transferFamily == 0xFFFFFFFFu) transferFamily = graphicsFamily;
    if (graphicsFamily == 0xFFFFFFFFu) throw std::runtime_error("no queues");
    std::printf("transfer family %u, graphics family %u%s\n", transferFamily, graphicsFamily,
                transferFamily == graphicsFamily ? " (same: transfer is illustrative only)" : "");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfos[2]{};
    queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfos[0].queueFamilyIndex = transferFamily;
    queueInfos[0].queueCount = 1;
    queueInfos[0].pQueuePriorities = &priority;
    std::uint32_t queueInfoCount = 1;
    if (graphicsFamily != transferFamily) {
        queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfos[1].queueFamilyIndex = graphicsFamily;
        queueInfos[1].queueCount = 1;
        queueInfos[1].pQueuePriorities = &priority;
        queueInfoCount = 2;
    }
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = queueInfoCount;
    deviceInfo.pQueueCreateInfos = queueInfos;
    VkDevice device = VK_NULL_HANDLE;
    check(vkCreateDevice(gpu, &deviceInfo, nullptr, &device), "vkCreateDevice");
    VkQueue transferQueue = VK_NULL_HANDLE, graphicsQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, transferFamily, 0, &transferQueue);
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    const auto findMemory = [&](std::uint32_t bits, VkMemoryPropertyFlags flags) {
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) != 0u && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        }
        throw std::runtime_error("no suitable memory type");
    };

    // Shared buffer: written on the transfer path, read on the graphics path.
    constexpr VkDeviceSize kBytes = static_cast<VkDeviceSize>(kDwords) * 4u;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = kBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    // Concurrent sharing avoids an ownership transfer when both families are the
    // same; with two families we still use EXCLUSIVE + explicit transfer below
    // to demonstrate the required barrier (concurrent sharing would hide it).
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    check(vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, buffer, bufferMemory, 0), "vkBindBufferMemory");

    auto makePool = [&](std::uint32_t family) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        VkCommandPool pool = VK_NULL_HANDLE;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        return pool;
    };
    VkCommandPool transferPool = makePool(transferFamily);
    VkCommandPool graphicsPool = makePool(graphicsFamily);
    auto oneTime = [&](VkCommandPool pool, VkCommandBuffer* commands) {
        VkCommandBufferAllocateInfo cmdAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmdAlloc.commandPool = pool;
        cmdAlloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &cmdAlloc, commands), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(*commands, &beginInfo), "vkBeginCommandBuffer");
    };

    // Step 1 (transfer path): fill with pattern A, then RELEASE ownership to the
    // graphics family (a no-op barrier when families coincide, still validated).
    VkCommandBuffer fillCommands = VK_NULL_HANDLE;
    oneTime(transferPool, &fillCommands);
    vkCmdFillBuffer(fillCommands, buffer, 0, kBytes, kPatternA);
    VkBufferMemoryBarrier release{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    release.dstAccessMask = 0;
    release.srcQueueFamilyIndex = transferFamily;
    release.dstQueueFamilyIndex = graphicsFamily;
    release.buffer = buffer;
    release.offset = 0;
    release.size = kBytes;
    vkCmdPipelineBarrier(fillCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1, &release, 0, nullptr);
    check(vkEndCommandBuffer(fillCommands), "vkEndCommandBuffer");
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore transferDone = VK_NULL_HANDLE;
    check(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &transferDone), "vkCreateSemaphore");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "vkCreateFence");
    VkSubmitInfo transferSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    transferSubmit.commandBufferCount = 1;
    transferSubmit.pCommandBuffers = &fillCommands;
    transferSubmit.signalSemaphoreCount = 1;
    transferSubmit.pSignalSemaphores = &transferDone;
    check(vkQueueSubmit(transferQueue, 1, &transferSubmit, VK_NULL_HANDLE), "vkQueueSubmit transfer");

    // Step 2 (graphics path): wait on the semaphore, ACQUIRE ownership, overwrite
    // with pattern B, signal completion for the CPU fence wait.
    VkCommandBuffer useCommands = VK_NULL_HANDLE;
    oneTime(graphicsPool, &useCommands);
    VkBufferMemoryBarrier acquire{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    acquire.srcQueueFamilyIndex = transferFamily;
    acquire.dstQueueFamilyIndex = graphicsFamily;
    acquire.buffer = buffer;
    acquire.offset = 0;
    acquire.size = kBytes;
    vkCmdPipelineBarrier(useCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &acquire, 0, nullptr);
    vkCmdFillBuffer(useCommands, buffer, 0, kBytes, kPatternB);
    VkBufferMemoryBarrier hostVisible{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    hostVisible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hostVisible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hostVisible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostVisible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hostVisible.buffer = buffer;
    hostVisible.offset = 0;
    hostVisible.size = kBytes;
    vkCmdPipelineBarrier(useCommands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostVisible, 0, nullptr);
    check(vkEndCommandBuffer(useCommands), "vkEndCommandBuffer");
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo useSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    useSubmit.waitSemaphoreCount = 1;
    useSubmit.pWaitSemaphores = &transferDone;
    useSubmit.pWaitDstStageMask = &waitStage;
    useSubmit.commandBufferCount = 1;
    useSubmit.pCommandBuffers = &useCommands;
    check(vkQueueSubmit(graphicsQueue, 1, &useSubmit, fence), "vkQueueSubmit graphics");
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10000000000ull), "vkWaitForFences");

    void* mapped = nullptr;
    check(vkMapMemory(device, bufferMemory, 0, kBytes, 0, &mapped), "vkMapMemory");
    const auto* words = static_cast<const std::uint32_t*>(mapped);
    bool ok = true;
    for (std::uint32_t i = 0; i < kDwords; ++i) {
        if (words[i] != kPatternB) {
            std::fprintf(stderr, "mismatch at dword %u: 0x%x\n", i, words[i]);
            ok = false;
            break;
        }
    }
    vkUnmapMemory(device, bufferMemory);
    std::printf("sync chain (semaphore + fence + ownership transfer) %s\n", ok ? "OK" : "MISMATCH");
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    return ok ? 0 : 1;
}
