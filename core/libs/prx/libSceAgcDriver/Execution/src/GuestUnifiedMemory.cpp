#include "prx/libSceAgcDriver/Execution/include/GuestUnifiedMemory.hpp"

#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::GuestUnifiedMemory {

namespace {

bool EnvOff(const char* name) {
    static bool cached[2] = {false, false};
    // Tiny cache: index 0 = APS5_NO_UMA, 1 = APS5_UMA_FAULTS.
    const int index = std::strcmp(name, "APS5_NO_UMA") == 0 ? 0 : 1;
    static bool init[2] = {false, false};
    if (!init[index]) {
        cached[index] = std::getenv(name) != nullptr;
        init[index] = true;
    }
    return cached[index];
}

struct Counters {
    std::atomic<std::uint64_t> cpuToGpuBytes{0};
    std::atomic<std::uint64_t> gpuToCpuBytes{0};
    std::atomic<std::uint64_t> copyBytes{0};
    std::atomic<std::uint64_t> copies{0};
    std::atomic<std::uint64_t> reusedBytes{0};
    std::atomic<std::uint64_t> reused{0};
    std::atomic<std::uint64_t> faultsServiced{0};
    std::atomic<std::uint64_t> gpuWaitMsMilli{0};
    std::atomic<std::uint64_t> fastSkip[static_cast<std::size_t>(FastSkip::Count)]{};
};

Counters& GlobalCounters() {
    static Counters counters;
    return counters;
}

struct FaultQueue {
    std::mutex mutex;
    std::queue<FaultRequest> queue;
};

FaultQueue& GlobalFaultQueue() {
    static FaultQueue queue;
    return queue;
}

}  // namespace

bool UmaEnabled() {
    return !EnvOff("APS5_NO_UMA");
}

bool FaultsEnabled() {
    return EnvOff("APS5_UMA_FAULTS");
}

Capabilities ProbeCapabilities(const VkPhysicalDeviceMemoryProperties& memory,
                               const VkPhysicalDeviceLimits& limits, bool hasBDA) {
    Capabilities caps;
    caps.hasBufferDeviceAddress = hasBDA;
    caps.nonCoherentAtomSize = limits.nonCoherentAtomSize != 0 ? limits.nonCoherentAtomSize : 1;
    caps.hasMemoryBudget = false;  // Wired separately when the extension is enabled.
    for (std::uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        bool heapHasBarType = false;
        for (std::uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
            const auto flags = memory.memoryTypes[type].propertyFlags;
            if (memory.memoryTypes[type].heapIndex != heap) continue;
            if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) caps.hasHostCoherent = true;
            if ((flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) caps.hasHostCached = true;
            const bool bar = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
                             (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
            if (bar) heapHasBarType = true;
        }
        if (heapHasBarType) {
            caps.hasDeviceLocalHostVisible = true;
            // Keep the largest BAR heap; a 256MiB default BAR stays 256MiB here,
            // never inflated into "full VRAM".
            if (memory.memoryHeaps[heap].size > caps.barBytes) {
                caps.barBytes = memory.memoryHeaps[heap].size;
                caps.barHeapIndex = heap;
            }
        }
    }
    caps.resizableBar = caps.barBytes >= (1ull << 30);
    return caps;
}

const char* StrategyName(BackingStrategy strategy) {
    return strategy == BackingStrategy::DirectMap ? "direct" : "separate";
}

BackingStrategy ChooseBacking(const Capabilities& caps, std::uint64_t size, bool cpuReadsOften,
                              bool sequentialWrite) {
    if (size == 0) return BackingStrategy::SeparateBacking;
    if (!caps.hasDeviceLocalHostVisible) return BackingStrategy::SeparateBacking;
    // Random CPU reads over PCIe stall: never direct-map read-mostly ranges.
    if (cpuReadsOften) return BackingStrategy::SeparateBacking;
    // Only sequential-write streaming benefits from write-combined BAR.
    if (!sequentialWrite) return BackingStrategy::SeparateBacking;
    // Tiny default BAR (256MiB) cannot back the whole arena: direct-map only
    // small streaming ranges, never the world.
    if (!caps.resizableBar && size > (256ull << 20)) return BackingStrategy::SeparateBacking;
    return BackingStrategy::DirectMap;
}

std::uint64_t UnifiedWindow::BlockBase(std::uint64_t address) {
    return address & ~static_cast<std::uint64_t>(kBlockBytes - 1);
}

void UnifiedWindow::Register(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        auto& block = blocks[base];
        block.owner = Owner::Shared;
        block.dirty = false;
    }
}

void UnifiedWindow::Unregister(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end;) {
        auto found = blocks.find(base);
        if (found == blocks.end()) {
            base += kBlockBytes;
            continue;
        }
        blocks.erase(found);
        if (base < BlockBase(begin)) break;  // Overflow guard.
        base += kBlockBytes;
    }
}

void UnifiedWindow::NoteCpuWrite(std::uint64_t begin, std::uint64_t end) {
    if (begin >= end) return;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        auto& block = blocks[base];
        block.owner = Owner::Cpu;
        block.dirty = true;
        ++block.version;
    }
}

void UnifiedWindow::NoteGpuWrite(std::uint64_t begin, std::uint64_t end, std::uint64_t submission) {
    if (begin >= end) return;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        auto& block = blocks[base];
        block.owner = Owner::Gpu;
        block.dirty = true;
        ++block.version;
        if (submission > block.lastWriterSubmission) block.lastWriterSubmission = submission;
    }
}

bool UnifiedWindow::NeedsUploadForGpu(std::uint64_t begin, std::uint64_t end) const {
    if (begin >= end) return false;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        const auto found = blocks.find(base);
        // Unknown range: caller must prove clean by compare, never assume.
        if (found == blocks.end()) return true;
        if (found->second.owner == Owner::Cpu && found->second.dirty) return true;
    }
    return false;
}

bool UnifiedWindow::NeedsDownloadForCpu(std::uint64_t begin, std::uint64_t end) const {
    if (begin >= end) return false;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        const auto found = blocks.find(base);
        if (found == blocks.end()) return true;
        if (found->second.owner == Owner::Gpu && found->second.dirty) return true;
    }
    return false;
}

void UnifiedWindow::MarkSynchronized(std::uint64_t begin, std::uint64_t end, Owner owner) {
    if (begin >= end) return;
    std::lock_guard lock(mutex);
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        auto& block = blocks[base];
        block.owner = owner;
        block.dirty = false;
        ++block.version;
    }
}

std::uint64_t UnifiedWindow::PendingSubmission(std::uint64_t begin, std::uint64_t end) const {
    if (begin >= end) return 0;
    std::lock_guard lock(mutex);
    std::uint64_t pending = 0;
    for (auto base = BlockBase(begin); base < end; base += kBlockBytes) {
        const auto found = blocks.find(base);
        if (found == blocks.end()) continue;
        if (found->second.lastWriterSubmission > pending) pending = found->second.lastWriterSubmission;
    }
    return pending;
}

std::size_t UnifiedWindow::BlockCount() const {
    std::lock_guard lock(mutex);
    return blocks.size();
}

bool TryEnqueueFault(const FaultRequest& request) {
    if (!FaultsEnabled() || !UmaEnabled()) return false;
    auto& queue = GlobalFaultQueue();
    std::lock_guard lock(queue.mutex);
    if (queue.queue.size() >= 4096) return false;
    queue.queue.push(request);
    return true;
}

bool TryDequeueFault(FaultRequest& out) {
    auto& queue = GlobalFaultQueue();
    std::lock_guard lock(queue.mutex);
    if (queue.queue.empty()) return false;
    out = queue.queue.front();
    queue.queue.pop();
    return true;
}

std::size_t FaultQueueDepth() {
    auto& queue = GlobalFaultQueue();
    std::lock_guard lock(queue.mutex);
    return queue.queue.size();
}

void NoteCpuToGpu(std::uint64_t bytes) {
    if (!UmaEnabled() || bytes == 0) return;
    GlobalCounters().cpuToGpuBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteGpuToCpu(std::uint64_t bytes) {
    if (!UmaEnabled() || bytes == 0) return;
    GlobalCounters().gpuToCpuBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteCopy(std::uint64_t bytes) {
    if (!UmaEnabled()) return;
    GlobalCounters().copies.fetch_add(1, std::memory_order_relaxed);
    GlobalCounters().copyBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteReuse(std::uint64_t bytes) {
    if (!UmaEnabled()) return;
    GlobalCounters().reused.fetch_add(1, std::memory_order_relaxed);
    GlobalCounters().reusedBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void NoteFaultServiced() {
    if (!UmaEnabled()) return;
    GlobalCounters().faultsServiced.fetch_add(1, std::memory_order_relaxed);
}

void NoteFastSkip(FastSkip reason) {
    if (!UmaEnabled()) return;
    const auto index = static_cast<std::size_t>(reason);
    if (index < static_cast<std::size_t>(FastSkip::Count))
        GlobalCounters().fastSkip[index].fetch_add(1, std::memory_order_relaxed);
}

void NoteGpuWaitMs(double ms) {
    if (!UmaEnabled() || ms <= 0.0) return;
    const auto milli = static_cast<std::uint64_t>(ms * 1000.0);
    GlobalCounters().gpuWaitMsMilli.fetch_add(milli, std::memory_order_relaxed);
}

Snapshot TakeSnapshot() {
    Snapshot snapshot;
    auto& counters = GlobalCounters();
    snapshot.cpuToGpuBytes = counters.cpuToGpuBytes.load(std::memory_order_relaxed);
    snapshot.gpuToCpuBytes = counters.gpuToCpuBytes.load(std::memory_order_relaxed);
    snapshot.copyBytes = counters.copyBytes.load(std::memory_order_relaxed);
    snapshot.copies = counters.copies.load(std::memory_order_relaxed);
    snapshot.reusedBytes = counters.reusedBytes.load(std::memory_order_relaxed);
    snapshot.reused = counters.reused.load(std::memory_order_relaxed);
    snapshot.faultsServiced = counters.faultsServiced.load(std::memory_order_relaxed);
    snapshot.gpuWaitMs =
        static_cast<double>(counters.gpuWaitMsMilli.load(std::memory_order_relaxed)) / 1000.0;
    snapshot.fastNoGeneration =
        counters.fastSkip[static_cast<std::size_t>(FastSkip::NoGeneration)].load(std::memory_order_relaxed);
    snapshot.fastTexelsChanged =
        counters.fastSkip[static_cast<std::size_t>(FastSkip::TexelsChanged)].load(std::memory_order_relaxed);
    snapshot.fastKeysUnproved =
        counters.fastSkip[static_cast<std::size_t>(FastSkip::KeysUnproved)].load(std::memory_order_relaxed);
    snapshot.fastKeysDiffer =
        counters.fastSkip[static_cast<std::size_t>(FastSkip::KeysDiffer)].load(std::memory_order_relaxed);
    snapshot.fastOtherPending =
        counters.fastSkip[static_cast<std::size_t>(FastSkip::OtherPending)].load(std::memory_order_relaxed);
    return snapshot;
}

void MaybeReport(const Capabilities* caps) {
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    const auto snapshot = TakeSnapshot();
    if (caps != nullptr) {
        ProfilePrint_nid_no_patch(
            "[uma] cpu->gpu %.1fMiB gpu->cpu %.1fMiB copies %llu/%.1fMiB reused %llu/%.1fMiB "
            "faults %llu gpuWait %.1fms fastSkip noGen %llu changed %llu keysUnproved %llu "
            "keysDiffer %llu otherPending %llu | BAR %lluMiB%s bda %d budget %d atom %zu\n",
            snapshot.cpuToGpuBytes / 1048576.0, snapshot.gpuToCpuBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.copies), snapshot.copyBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.reused), snapshot.reusedBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.faultsServiced), snapshot.gpuWaitMs,
            static_cast<unsigned long long>(snapshot.fastNoGeneration),
            static_cast<unsigned long long>(snapshot.fastTexelsChanged),
            static_cast<unsigned long long>(snapshot.fastKeysUnproved),
            static_cast<unsigned long long>(snapshot.fastKeysDiffer),
            static_cast<unsigned long long>(snapshot.fastOtherPending),
            static_cast<unsigned long long>(caps->barBytes >> 20), caps->resizableBar ? " (ReBAR)" : "",
            caps->hasBufferDeviceAddress ? 1 : 0, caps->hasMemoryBudget ? 1 : 0,
            caps->nonCoherentAtomSize);
    } else {
        ProfilePrint_nid_no_patch(
            "[uma] cpu->gpu %.1fMiB gpu->cpu %.1fMiB copies %llu/%.1fMiB reused %llu/%.1fMiB "
            "faults %llu gpuWait %.1fms fastSkip noGen %llu changed %llu keysUnproved %llu "
            "keysDiffer %llu otherPending %llu\n",
            snapshot.cpuToGpuBytes / 1048576.0, snapshot.gpuToCpuBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.copies), snapshot.copyBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.reused), snapshot.reusedBytes / 1048576.0,
            static_cast<unsigned long long>(snapshot.faultsServiced), snapshot.gpuWaitMs,
            static_cast<unsigned long long>(snapshot.fastNoGeneration),
            static_cast<unsigned long long>(snapshot.fastTexelsChanged),
            static_cast<unsigned long long>(snapshot.fastKeysUnproved),
            static_cast<unsigned long long>(snapshot.fastKeysDiffer),
            static_cast<unsigned long long>(snapshot.fastOtherPending));
    }
}

}  // namespace AgcDriver::GuestUnifiedMemory
