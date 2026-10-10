// Demo 04 — buffer copies and bounds discipline (no window).
//
// Hypothesis: exact-size copies round-trip byte-identical data; any overrun is
// undefined behavior that validation may flag but cannot prevent.
// Run: 04_copies.exe
// Expected: "round-trip OK (N bytes)"; exit 0. The DELIBERATELY_WRONG_* blocks
//   are compiled out by default (ENABLE_OOB_DEMO=0); enabling them is FORBIDDEN
//   here (mission rule: no intentional OOB that can hang/reset the GPU) —
//   overrun behavior is covered by static analysis + demo 08 under validation.
// Actual: NOT RUN (no SDK headers to build against).
// Limits: one queue, device-local destination, host-visible source+readback.

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

constexpr std::uint64_t kBytes = 1u << 20; // 1 MiB pattern buffer

} // namespace

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gpu_research/04_copies";
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
    bool found = false;
    for (std::uint32_t i = 0; i < familyCount; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0u) {
            family = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (std::uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                family = i;
                found = true;
                break;
            }
        }
    }
    if (!found) throw std::runtime_error("no transfer/graphics queue");
    std::printf("using queue family %u (%s)\n", family, ((families[family].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0u) ? "transfer" : "graphics");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = family;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
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

    auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags, VkBuffer* buffer,
                          VkDeviceMemory* memoryOut) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &bufferInfo, nullptr, buffer), "vkCreateBuffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, *buffer, &req);
        // Bounds discipline, part 1: the allocation must cover the whole copy.
        if (req.size < size) throw std::runtime_error("allocation smaller than the copy");
        VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = findMemory(req.memoryTypeBits, flags);
        check(vkAllocateMemory(device, &allocInfo, nullptr, memoryOut), "vkAllocateMemory");
        check(vkBindBufferMemory(device, *buffer, *memoryOut, 0), "vkBindBufferMemory");
    };

    VkBuffer src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE, readback = VK_NULL_HANDLE;
    VkDeviceMemory srcMemory = VK_NULL_HANDLE, dstMemory = VK_NULL_HANDLE, readbackMemory = VK_NULL_HANDLE;
    makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &src, &srcMemory);
    makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &dst, &dstMemory);
    makeBuffer(kBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &readback, &readbackMemory);

    void* mapped = nullptr;
    check(vkMapMemory(device, srcMemory, 0, kBytes, 0, &mapped), "vkMapMemory src");
    auto* words = static_cast<std::uint32_t*>(mapped);
    for (std::uint64_t i = 0; i < kBytes / 4; ++i) words[i] = static_cast<std::uint32_t>(i * 2654435761ull);
    vkUnmapMemory(device, srcMemory);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &allocInfo, &commands), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commands, &beginInfo), "vkBeginCommandBuffer");
    // Bounds discipline, part 2: EVERY copy region must lie inside BOTH buffers.
    // {srcOffset=0, dstOffset=0, size=kBytes} is exactly the allocation size.
    const VkBufferCopy toDevice{0, 0, kBytes};
    vkCmdCopyBuffer(commands, src, dst, 1, &toDevice);
    VkBufferMemoryBarrier ready{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    ready.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ready.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ready.buffer = dst;
    ready.offset = 0;
    ready.size = kBytes;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &ready, 0, nullptr);
    const VkBufferCopy toHost{0, 0, kBytes};
    vkCmdCopyBuffer(commands, dst, readback, 1, &toHost);
    check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

    check(vkMapMemory(device, readbackMemory, 0, kBytes, 0, &mapped), "vkMapMemory readback");
    const auto* checkWords = static_cast<const std::uint32_t*>(mapped);
    bool ok = true;
    for (std::uint64_t i = 0; i < kBytes / 4; ++i) {
        if (checkWords[i] != static_cast<std::uint32_t>(i * 2654435761ull)) {
            std::fprintf(stderr, "mismatch at word %llu: 0x%x\n", static_cast<unsigned long long>(i), checkWords[i]);
            ok = false;
            break;
        }
    }
    vkUnmapMemory(device, readbackMemory);
    std::printf("round-trip %s (%llu bytes)\n", ok ? "OK" : "MISMATCH", static_cast<unsigned long long>(kBytes));
    check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle");
    return ok ? 0 : 1;
}
