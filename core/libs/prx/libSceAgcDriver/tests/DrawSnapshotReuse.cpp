#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

struct MockDevice {
    std::uintptr_t next = 1;
    std::map<VkFence, bool> signaled;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* commands) {
    *commands = reinterpret_cast<VkCommandBuffer>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFreeCommandBuffers(VkDevice, VkCommandPool, std::uint32_t, const VkCommandBuffer*) {}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* fence) {
    *fence = reinterpret_cast<VkFence>(mock.next++);
    mock.signaled[*fence] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks*) {
    mock.signaled.erase(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL mockGetFenceStatus(VkDevice, VkFence fence) {
    return mock.signaled[fence] ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL mockWaitForFences(VkDevice, std::uint32_t count, const VkFence* fences, VkBool32, std::uint64_t) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled[fences[i]] = true;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetFences(VkDevice, std::uint32_t count, const VkFence* fences) {
    for (std::uint32_t i = 0; i < count; ++i) mock.signaled[fences[i]] = false;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEndCommandBuffer(VkCommandBuffer) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockCmdUpdateBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t, const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBeginQuery(VkCommandBuffer, VkQueryPool, std::uint32_t, VkQueryControlFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdEndQuery(VkCommandBuffer, VkQueryPool, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdResetQueryPool(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t) {}
VKAPI_ATTR void VKAPI_CALL mockCmdCopyQueryPoolResults(VkCommandBuffer, VkQueryPool, std::uint32_t, std::uint32_t, VkBuffer, VkDeviceSize, VkDeviceSize, VkQueryResultFlags) {}
VKAPI_ATTR void VKAPI_CALL mockCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL mockCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, std::uint32_t, std::uint32_t, const void*) {}
VKAPI_ATTR void VKAPI_CALL mockCmdDispatch(VkCommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t) {}

VKAPI_ATTR VkResult VKAPI_CALL mockCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* buffer) {
    *buffer = reinterpret_cast<VkBuffer>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* requirements) {
    requirements->size = 65536;
    requirements->alignment = 256;
    requirements->memoryTypeBits = 0x1;
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    *memory = reinterpret_cast<VkDeviceMemory>(mock.next++);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize size, VkMemoryMapFlags, void** data) {
    const std::size_t bytes = size == VK_WHOLE_SIZE ? 65536 : static_cast<std::size_t>(size);
    *data = std::malloc(bytes);
    return *data != nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL mockUnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR void VKAPI_CALL mockDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL mockFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) {}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    static const std::map<std::string_view, PFN_vkVoidFunction> table{
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(mockFreeCommandBuffers)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(mockCreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyFence)},
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(mockGetFenceStatus)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(mockWaitForFences)},
        {"vkResetFences", reinterpret_cast<PFN_vkVoidFunction>(mockResetFences)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockBeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockEndCommandBuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(mockQueueSubmit)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCmdUpdateBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPipelineBarrier)},
        {"vkCmdBeginQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBeginQuery)},
        {"vkCmdEndQuery", reinterpret_cast<PFN_vkVoidFunction>(mockCmdEndQuery)},
        {"vkCmdResetQueryPool", reinterpret_cast<PFN_vkVoidFunction>(mockCmdResetQueryPool)},
        {"vkCmdCopyQueryPoolResults", reinterpret_cast<PFN_vkVoidFunction>(mockCmdCopyQueryPoolResults)},
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(mockCmdBindPipeline)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(mockCmdPushConstants)},
        {"vkCmdDispatch", reinterpret_cast<PFN_vkVoidFunction>(mockCmdDispatch)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockCreateBuffer)},
        {"vkGetBufferMemoryRequirements", reinterpret_cast<PFN_vkVoidFunction>(mockGetBufferMemoryRequirements)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(mockBindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockMapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(mockUnmapMemory)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(mockDestroyBuffer)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(mockFreeMemory)},
    };
    const auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

Context mockContext() {
    Context context{};
    context.device = reinterpret_cast<VkDevice>(mock.next++);
    context.queue = reinterpret_cast<VkQueue>(mock.next++);
    context.deviceProc = mockProc;
    context.memory.memoryTypeCount = 1;
    context.memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    context.memory.memoryTypes[0].heapIndex = 0;
    context.memory.memoryHeapCount = 1;
    return context;
}

void* AllocateWatched(std::size_t bytes) {
    constexpr std::size_t Block = 65536;
#ifdef _WIN32
    void* block = ::GuestArena::GuestArenaAllocate_nid_postfix(bytes, Block);
    ::GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
    throw std::runtime_error("watched allocation needs its platform branch");
#endif
    if (!AgcDriver::GuestMemory::Watched(reinterpret_cast<std::uint64_t>(block), bytes)) throw std::runtime_error("the test block is not watched");
    return block;
}

void ContentHashReuse() {
    mock = MockDevice{};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    Context context = mockContext();
    Recorder recorder(context);
    constexpr std::size_t bytes = 65536;
    void* memory = AllocateWatched(2 * bytes);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    const auto other = base + bytes;
    std::memset(memory, 0x5a, 2 * bytes);
    const auto registry = ::GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    const std::uint64_t first = AgcDriver::GuestMemory::CollectWrites(base, bytes);
    Expect(first != 0, "the test block is not collected");
    auto kept = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Expect(kept->Bytes().size() >= bytes, "the kept buffer is smaller than the snapshot");
    std::memcpy(kept->Bytes().data(), memory, bytes);
    recorder.KeepDrawSnapshot(base, bytes, first, registry, kept, Recorder::SnapshotUse::Storage, 7);
    Expect(recorder.ReusableDrawSnapshot(base, bytes).get() == kept.get(), "the exact lookup missed its own snapshot");
    AgcDriver::GuestMemory::MarkWritten(base, bytes);
    const std::uint64_t second = AgcDriver::GuestMemory::CollectWrites(base, bytes);
    Expect(second != 0, "the test block is not collected again");
    std::uint32_t derived = 0;
    auto reused = recorder.ReusableDrawSnapshotContent(base, bytes, second, Recorder::SnapshotUse::Storage, &derived);
    Expect(reused.get() == kept.get(), "the content lookup missed identical bytes over newer stamps");
    Expect(derived == 7, "the content lookup dropped the derived value");
    Expect(recorder.ReusableDrawSnapshot(base, bytes).get() == kept.get(), "the rescued entry is not exact-hittable");
    static_cast<std::uint8_t*>(memory)[100] ^= 0xff;
    AgcDriver::GuestMemory::MarkWritten(base, 1);
    Expect(recorder.ReusableDrawSnapshotContent(base, bytes, AgcDriver::GuestMemory::CollectWrites(base, bytes)) == nullptr, "the content lookup hit over changed bytes");
    auto otherKept = std::make_shared<Buffer>(context, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(otherKept->Bytes().data(), reinterpret_cast<const void*>(other), bytes);
    const std::uint64_t otherGen = AgcDriver::GuestMemory::CollectWrites(other, bytes);
    Expect(otherGen != 0, "the other block is not collected");
    recorder.KeepDrawSnapshot(other, bytes, otherGen, registry, otherKept);
    AgcDriver::GuestMemory::MarkWritten(other, bytes);
    Expect(recorder.ReusableDrawSnapshot(other, bytes) == nullptr, "the exact lookup hit over newer stamps");
}

}

int main() {
    try {
        ContentHashReuse();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d draw snapshot reuse checks failed\n", failures);
        return 1;
    }
    std::printf("draw snapshot reuse tests passed\n");
    return 0;
}
