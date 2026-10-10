#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTUNIFIEDMEMORY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_GUESTUNIFIEDMEMORY_HPP

// GuestUnifiedMemory: capability-driven UMA semantics over PC split memory.
//
// Guest model (PS5): one unified address space, CPU and GPU share addresses,
// aliasing is legal, order is defined by command submission + sync ops.
// Host reality: CPU RAM + GPU VRAM (+ optional BAR window), Vulkan allocations
// with granularity/alignment, explicit barriers, no hardware coherence.
//
// This layer does NOT claim BAR == UMA. It probes real capabilities and picks
// a strategy per range:
//   - DirectMap: use one backing (host import) when the device really exposes
//     DEVICE_LOCAL|HOST_VISIBLE for the range, alignment holds, and access is
//     mostly sequential writes (never random CPU reads from VRAM).
//   - SeparateBacking: two coherent views + minimal proven transfers otherwise.
// Dirty tracking defaults to software 64KiB stamps (same block as the write
// watch). OS page-fault tracking is investigated but OFF by default: Vulkan
// calls are not async-signal-safe, Unity/Mono own VirtualProtect, and 4KiB
// faults fragment the 64KiB coalescing. See FaultsEnabled().

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::GuestUnifiedMemory {

constexpr std::size_t kBlockBytes = 65536;
constexpr std::uint32_t kUnknownHeap = 0xFFFFFFFFu;

struct Capabilities {
    bool hasDeviceLocalHostVisible = false;
    std::uint64_t barBytes = 0;
    std::uint32_t barHeapIndex = kUnknownHeap;
    bool hasHostCoherent = false;
    bool hasHostCached = false;
    bool hasBufferDeviceAddress = false;
    bool hasMemoryBudget = false;
    std::size_t nonCoherentAtomSize = 1;
    // True only when the BAR window is large enough to matter (>=1GiB).
    // A 256MiB default BAR is reported, never treated as full-VRAM ReBAR.
    bool resizableBar = false;
};

// Probe from already-queried properties only: no new Vulkan calls, no SID.
// `hasBDA` comes from Context::bufferDeviceAddress; budget presence is
// reported by the caller when VK_EXT_memory_budget is enabled (default false
// here: honest "unknown" until wired to the device features query).
Capabilities ProbeCapabilities(const VkPhysicalDeviceMemoryProperties& memory,
                               const VkPhysicalDeviceLimits& limits, bool hasBDA);

enum class BackingStrategy { DirectMap, SeparateBacking };

const char* StrategyName(BackingStrategy strategy);

// Capability-driven choice. Never picks DirectMap without a real BAR type,
// and never for ranges the CPU reads randomly (reads over PCIe stall).
BackingStrategy ChooseBacking(const Capabilities& caps, std::uint64_t size, bool cpuReadsOften,
                              bool sequentialWrite);

enum class Owner { Cpu, Gpu, Shared };

struct BlockState {
    Owner owner = Owner::Shared;
    std::uint64_t version = 0;
    bool dirty = false;
    // Last GPU submission serial that wrote the block (grouped dependency,
    // never a semaphore per page).
    std::uint64_t lastWriterSubmission = 0;
};

// Interval registry at kBlockBytes granularity. Thread-safe. Lives for
// address ranges the translator pins; unregistered ranges answer "unknown"
// (caller falls back to byte compares, never assumes clean).
class UnifiedWindow {
public:
    void Register(std::uint64_t begin, std::uint64_t end);
    void Unregister(std::uint64_t begin, std::uint64_t end);
    void NoteCpuWrite(std::uint64_t begin, std::uint64_t end);
    void NoteGpuWrite(std::uint64_t begin, std::uint64_t end, std::uint64_t submission);
    // True when a transfer is proven necessary (dirty owner on the other side).
    bool NeedsUploadForGpu(std::uint64_t begin, std::uint64_t end) const;
    bool NeedsDownloadForCpu(std::uint64_t begin, std::uint64_t end) const;
    void MarkSynchronized(std::uint64_t begin, std::uint64_t end, Owner owner);
    // Max writer submission over the range (0 = none): the caller waits for
    // that submission (batch fence/timeline), never per-page semaphores.
    std::uint64_t PendingSubmission(std::uint64_t begin, std::uint64_t end) const;
    std::size_t BlockCount() const;

private:
    static std::uint64_t BlockBase(std::uint64_t address);
    mutable std::mutex mutex;
    std::map<std::uint64_t, BlockState> blocks;
};

// Fauilt-service queue. The fault handler (VEH/SIGSEGV) must NEVER call
// Vulkan: it only enqueues {address,size,isWrite}. A dedicated service thread
// (owned by the caller, not started here) dequeues, waits for the pending
// submission, performs exactly one ranged copy + barriers, unprotects, and
// resumes the faulting thread. Disabled by default (APS5_UMA_FAULTS=1 opts
// into the prototype for explicitly registered ranges only).
struct FaultRequest {
    std::uint64_t address = 0;
    std::size_t bytes = 0;
    bool isWrite = false;
};

bool FaultsEnabled();
bool UmaEnabled();
bool TryEnqueueFault(const FaultRequest& request);
bool TryDequeueFault(FaultRequest& out);
std::size_t FaultQueueDepth();

// Fast-path abandonment reasons (StorageTexture::Refresh): which gate sent the
// lookup to the slow path. Reported on the [uma] line; the fix follows the top
// reason instead of guessing.
enum class FastSkip { NoGeneration, TexelsChanged, KeysUnproved, KeysDiffer, OtherPending, Count };
void NoteFastSkip(FastSkip reason);
// Counters (all relaxed atomics; reported, never used for correctness).
void NoteCpuToGpu(std::uint64_t bytes);
void NoteGpuToCpu(std::uint64_t bytes);
void NoteCopy(std::uint64_t bytes);
void NoteReuse(std::uint64_t bytes);
void NoteFaultServiced();
void NoteGpuWaitMs(double ms);
struct Snapshot {
    std::uint64_t cpuToGpuBytes = 0;
    std::uint64_t gpuToCpuBytes = 0;
    std::uint64_t copyBytes = 0;
    std::uint64_t copies = 0;
    std::uint64_t reusedBytes = 0;
    std::uint64_t reused = 0;
    std::uint64_t faultsServiced = 0;
    double gpuWaitMs = 0.0;
    std::uint64_t fastNoGeneration = 0;
    std::uint64_t fastTexelsChanged = 0;
    std::uint64_t fastKeysUnproved = 0;
    std::uint64_t fastKeysDiffer = 0;
    std::uint64_t fastOtherPending = 0;
};
Snapshot TakeSnapshot();
// [uma] line at most every 10s (APS5_PROFILE_DRAW gates like the rest).
void MaybeReport(const Capabilities* caps = nullptr);

}  // namespace AgcDriver::GuestUnifiedMemory

#endif
