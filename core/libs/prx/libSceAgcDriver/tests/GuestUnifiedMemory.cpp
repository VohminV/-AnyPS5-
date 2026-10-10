// GuestUnifiedMemory equivalence tests: UMA semantics over split host memory.
// Covers task section 9 without a GPU: cpu->gpu, gpu->cpu, reread clean,
// partial ranges, overlapping maps, address reuse, submission grouping,
// fault-queue safety default, capability probe honesty (256MiB BAR != ReBAR).
#include "prx/libSceAgcDriver/Execution/include/GuestUnifiedMemory.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::GuestUnifiedMemory;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

VkPhysicalDeviceMemoryProperties FakeMemory(bool bar, std::uint64_t barBytes) {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryHeapCount = bar ? 2 : 1;
    memory.memoryHeaps[0].size = 8ull << 30;
    memory.memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    if (bar) {
        memory.memoryHeaps[1].size = barBytes;
        memory.memoryHeaps[1].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    }
    memory.memoryTypeCount = bar ? 2 : 1;
    memory.memoryTypes[0].heapIndex = 0;
    memory.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (bar) {
        memory.memoryTypes[1].heapIndex = 1;
        memory.memoryTypes[1].propertyFlags =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
    return memory;
}

VkPhysicalDeviceLimits FakeLimits() {
    VkPhysicalDeviceLimits limits{};
    limits.nonCoherentAtomSize = 64;
    limits.maxMemoryAllocationCount = 4096;
    return limits;
}

void CheckProbeHonesty() {
    // No BAR type at all: never direct-map capable.
    auto none = ProbeCapabilities(FakeMemory(false, 0), FakeLimits(), true);
    Require(!none.hasDeviceLocalHostVisible && none.barBytes == 0 && !none.resizableBar,
            "missing BAR reads as present");
    // 256MiB default BAR: reported, NOT ReBAR.
    auto small = ProbeCapabilities(FakeMemory(true, 256ull << 20), FakeLimits(), true);
    Require(small.hasDeviceLocalHostVisible && small.barBytes == (256ull << 20) && !small.resizableBar,
            "256MiB BAR inflated into ReBAR");
    // 8GiB BAR: ReBAR.
    auto big = ProbeCapabilities(FakeMemory(true, 8ull << 30), FakeLimits(), true);
    Require(big.hasDeviceLocalHostVisible && big.resizableBar, "8GiB BAR not recognized as ReBAR");
    Require(big.nonCoherentAtomSize == 64, "atom size not carried");
    // Strategy: random CPU reads never direct-map, even with ReBAR.
    Require(ChooseBacking(big, 1ull << 20, true, true) == BackingStrategy::SeparateBacking,
            "read-mostly range offered direct BAR map");
    Require(ChooseBacking(big, 1ull << 20, false, true) == BackingStrategy::DirectMap,
            "sequential stream refused direct BAR map");
    Require(ChooseBacking(small, 2ull << 30, false, true) == BackingStrategy::SeparateBacking,
            "2GiB range offered 256MiB BAR map");
    Require(ChooseBacking(none, 4096, false, true) == BackingStrategy::SeparateBacking,
            "direct map without any BAR type");
}

void CheckCpuToGpuToCpu() {
    UnifiedWindow window;
    const std::uint64_t base = 0x200000000ull;
    window.Register(base, base + 2 * kBlockBytes);
    // Freshly registered: clean both directions (shared, not dirty).
    Require(!window.NeedsUploadForGpu(base, base + kBlockBytes), "clean range wants upload");
    Require(!window.NeedsDownloadForCpu(base, base + kBlockBytes), "clean range wants download");
    // CPU write -> GPU needs upload, CPU does not need download.
    window.NoteCpuWrite(base, base + kBlockBytes);
    Require(window.NeedsUploadForGpu(base, base + kBlockBytes), "cpu write hidden from gpu");
    Require(!window.NeedsDownloadForCpu(base, base + kBlockBytes), "cpu write reads as gpu-owned");
    window.MarkSynchronized(base, base + kBlockBytes, Owner::Shared);
    Require(!window.NeedsUploadForGpu(base, base + kBlockBytes), "synced range still wants upload");
    // GPU write(submission 7) -> CPU needs download with that dependency.
    window.NoteGpuWrite(base, base + kBlockBytes, 7);
    Require(window.NeedsDownloadForCpu(base, base + kBlockBytes), "gpu write hidden from cpu");
    Require(window.PendingSubmission(base, base + kBlockBytes) == 7, "submission not grouped");
    Require(!window.NeedsUploadForGpu(base, base + kBlockBytes), "gpu write reads as cpu-owned");
    window.MarkSynchronized(base, base + kBlockBytes, Owner::Shared);
    Require(!window.NeedsDownloadForCpu(base, base + kBlockBytes), "synced range still wants download");
}

void CheckPartialAndOverlap() {
    UnifiedWindow window;
    const std::uint64_t base = 0x210000000ull;
    window.Register(base, base + 4 * kBlockBytes);
    // Partial CPU write: only that block uploads.
    window.NoteCpuWrite(base + kBlockBytes, base + 2 * kBlockBytes);
    Require(!window.NeedsUploadForGpu(base, base + kBlockBytes), "untouched head wants upload");
    Require(window.NeedsUploadForGpu(base + kBlockBytes, base + 2 * kBlockBytes),
            "written block skips upload");
    Require(!window.NeedsUploadForGpu(base + 2 * kBlockBytes, base + 4 * kBlockBytes),
            "untouched tail wants upload");
    // Overlapping GPU write across the boundary keeps both sides honest.
    window.NoteGpuWrite(base + kBlockBytes, base + 3 * kBlockBytes, 11);
    Require(window.NeedsDownloadForCpu(base, base + 2 * kBlockBytes), "overlap hides gpu block");
    Require(window.NeedsDownloadForCpu(base + 2 * kBlockBytes, base + 3 * kBlockBytes),
            "overlap tail hidden");
    Require(!window.NeedsDownloadForCpu(base + 3 * kBlockBytes, base + 4 * kBlockBytes),
            "clean tail wants download");
    Require(window.PendingSubmission(base, base + 4 * kBlockBytes) == 11, "max submission not kept");
}

void CheckReuseAndLifetimes() {
    UnifiedWindow window;
    const std::uint64_t base = 0x220000000ull;
    window.Register(base, base + kBlockBytes);
    window.NoteCpuWrite(base, base + kBlockBytes);
    // Address reuse: unregister drops knowledge; re-register starts Shared.
    window.Unregister(base, base + kBlockBytes);
    window.Register(base, base + kBlockBytes);
    Require(!window.NeedsUploadForGpu(base, base + kBlockBytes), "reused address keeps stale dirt");
    Require(!window.NeedsDownloadForCpu(base, base + kBlockBytes), "reused address keeps stale owner");
    // Unknown range (never registered): must request proof, never assume clean.
    Require(window.NeedsUploadForGpu(base + 0x1000000, base + 0x1000000 + 4096),
            "unknown range assumed clean for gpu");
    Require(window.NeedsDownloadForCpu(base + 0x1000000, base + 0x1000000 + 4096),
            "unknown range assumed clean for cpu");
}

void CheckFaultQueueSafeDefault() {
    // Faults OFF by default: enqueue refused, service thread never called from
    // a signal handler. With APS5_UMA_FAULTS=1 the prototype queue works, but
    // the dequeue still happens on a service thread, never in-handler.
    FaultRequest ignored{};
    const bool enabled = FaultsEnabled();
    if (!enabled) {
        Require(!TryEnqueueFault(FaultRequest{0x230000000ull, 4096, false}),
                "fault accepted while disabled");
        Require(FaultQueueDepth() == 0, "fault queue non-empty while disabled");
    } else {
        Require(TryEnqueueFault(FaultRequest{0x230000000ull, 4096, false}), "fault refused");
        Require(TryDequeueFault(ignored) && ignored.address == 0x230000000ull, "fault lost");
    }
}

void CheckCounters() {
    const auto before = TakeSnapshot();
    NoteCpuToGpu(1ull << 20);
    NoteGpuToCpu(1ull << 19);
    NoteCopy(4096);
    NoteReuse(1ull << 20);
    const auto after = TakeSnapshot();
    Require(after.cpuToGpuBytes >= before.cpuToGpuBytes + (1ull << 20), "cpu->gpu not counted");
    Require(after.gpuToCpuBytes >= before.gpuToCpuBytes + (1ull << 19), "gpu->cpu not counted");
    Require(after.copies >= before.copies + 1 && after.copyBytes >= before.copyBytes + 4096,
            "copies not counted");
    Require(after.reusedBytes >= before.reusedBytes + (1ull << 20), "reuse not counted");
}

}  // namespace

int main() {
    try {
        CheckProbeHonesty();
        CheckCpuToGpuToCpu();
        CheckPartialAndOverlap();
        CheckReuseAndLifetimes();
        CheckFaultQueueSafeDefault();
        CheckCounters();
    } catch (const std::exception& error) {
        std::cerr << "uma test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "uma tests passed\n";
    return 0;
}
