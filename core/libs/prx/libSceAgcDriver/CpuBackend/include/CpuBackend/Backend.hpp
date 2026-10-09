#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "CpuBackend/Decode.hpp"
#include "CpuBackend/Exec.hpp"
#include "CpuBackend/Resources.hpp"
#include "CpuBackend/Schedule.hpp"
#include "CpuBackend/Simd.hpp"

namespace AgcDriver::CpuBackend {

enum class Mode : std::uint8_t {
    Legacy = 0,
    Fast = 1,
    Auto = 2
};

Mode ModeFromEnv();
const char* ModeName(Mode mode);
bool CanUseFast(std::span<const DecodedPacket> decoded);

struct SubmitStats {
    std::uint64_t packets = 0;
    std::uint64_t batches = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    std::uint64_t resourceHits = 0;
    std::uint64_t resourceMisses = 0;
    bool fellBack = false;
    Mode used = Mode::Legacy;
};

class Backend {
public:
    struct Options {
        Mode mode = Mode::Auto;
        std::size_t threads = 0;
        ResourceCache::Config resources{};
    };
    explicit Backend();
    explicit Backend(Options options);
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    void SetMode(Mode mode);
    Mode GetMode() const;
    Path ActivePath() const;
    SubmitStats Submit(std::span<const std::uint32_t> dwords);
    PhaseSnapshot Snapshot() const;
    void ResetStats();
    ResourceStats ResourceStatsSnapshot() const;
    std::uint64_t LastDigest() const;

private:
    Options options_;
    Mode mode_;
    Features features_;
    Path path_;
    ResourceCache cache_;
    PhaseTimer timer_;
    ThreadPool pool_;
    std::uint64_t lastDigest_ = 0;
    std::uint64_t tick_ = 0;
    std::uint64_t serial_ = 0;
    std::vector<DecodedPacket> decodeScratch_;
    std::vector<Batch> batchScratch_;
};

} // namespace AgcDriver::CpuBackend
