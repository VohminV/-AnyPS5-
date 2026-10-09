#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "CpuBackend/Decode.hpp"

namespace AgcDriver::CpuBackend {

enum class Phase : std::uint8_t {
    CpuPrepare = 0,
    Record = 1,
    GpuWait = 2
};

struct PhaseSnapshot {
    std::uint64_t cpuPrepareNs = 0;
    std::uint64_t recordNs = 0;
    std::uint64_t gpuWaitNs = 0;
    std::uint64_t decodeNs = 0;
    std::uint64_t resourceNs = 0;
    std::uint64_t prepareNs = 0;
    std::uint64_t syncNs = 0;
    std::uint64_t packets = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t submissions = 0;
};

class PhaseTimer {
public:
    void Reset();
    void Add(Phase phase, std::uint64_t ns);
    void AddDecode(std::uint64_t ns);
    void AddResource(std::uint64_t ns);
    void AddPrepare(std::uint64_t ns);
    void AddSync(std::uint64_t ns);
    void AddPackets(std::uint64_t count);
    void AddDraws(std::uint64_t count);
    void AddDispatches(std::uint64_t count);
    void AddSubmissions(std::uint64_t count);
    PhaseSnapshot Snapshot() const;

private:
    PhaseSnapshot snapshot_{};
};

enum class BatchKind : std::uint8_t {
    Single = 0,
    NopRun = 1,
    SetRun = 2
};

struct Batch {
    BatchKind kind = BatchKind::Single;
    std::size_t beginPacket = 0;
    std::size_t packetCount = 0;
    std::size_t beginDword = 0;
    std::size_t dwordCount = 0;
};

struct PlanStats {
    std::size_t batches = 0;
    std::size_t nopRuns = 0;
    std::size_t setRuns = 0;
    std::size_t singles = 0;
    std::size_t coalescedPackets = 0;
};

std::vector<Batch> PlanBatches(std::span<const DecodedPacket> packets, PlanStats* stats);

struct ExecCounters {
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t sets = 0;
    std::uint64_t nops = 0;
    std::uint64_t syncs = 0;
    std::uint64_t memories = 0;
    std::uint64_t controls = 0;
    std::uint64_t dwords = 0;
};

class Executor {
public:
    void Reset();
    void ApplyPackets(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded);
    void ApplyBatches(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded, std::span<const Batch> batches);
    std::uint64_t Digest() const;
    ExecCounters Counters() const;
    std::uint64_t RegisterStateHash() const;

private:
    std::uint64_t digest_ = 1469598103934665603ull;
    ExecCounters counters_{};
    std::uint64_t registerHash_ = 1469598103934665603ull;
    void MixPacket(std::uint32_t header, std::span<const std::uint32_t> words);
    void MixRegister(std::uint32_t space, std::uint32_t offset, std::uint32_t value);
};

std::uint64_t LegacyExecute(std::span<const std::uint32_t> dwords, ExecCounters* counters);
bool SameObservable(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded, std::span<const Batch> batches);

} // namespace AgcDriver::CpuBackend
