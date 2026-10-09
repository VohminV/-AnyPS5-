#include "CpuBackend/Backend.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::CpuBackend {

Mode ModeFromEnv() {
    const char* text = std::getenv("ANYPS5_CPU_BACKEND");
    if (text == nullptr) {
        return Mode::Auto;
    }
    if (std::strcmp(text, "legacy") == 0 || std::strcmp(text, "old") == 0) {
        return Mode::Legacy;
    }
    if (std::strcmp(text, "fast") == 0 || std::strcmp(text, "new") == 0) {
        return Mode::Fast;
    }
    return Mode::Auto;
}

const char* ModeName(Mode mode) {
    if (mode == Mode::Legacy) {
        return "legacy";
    }
    if (mode == Mode::Fast) {
        return "fast";
    }
    return "auto";
}

bool CanUseFast(std::span<const DecodedPacket> decoded) {
    for (const auto& packet : decoded) {
        if (packet.packetClass == PacketClass::Unknown) {
            return false;
        }
        if (packet.words == 0) {
            return false;
        }
    }
    return true;
}

Backend::Backend()
    : Backend(Options{}) {
}

Backend::Backend(Options options)
    : options_(options)
    , mode_(options.mode)
    , features_(Probe())
    , path_(Select(features_))
    , cache_(options.resources)
    , pool_(options.threads) {
    if (options_.threads == 0) {
        options_.threads = pool_.Threads();
    }
    if (path_ == Path::Avx2) {
        cache_.SetPath(2);
    } else if (path_ == Path::Sse) {
        cache_.SetPath(1);
    } else {
        cache_.SetPath(0);
    }
    decodeScratch_.reserve(4096);
    batchScratch_.reserve(4096);
}

void Backend::SetMode(Mode mode) {
    mode_ = mode;
}

Mode Backend::GetMode() const {
    return mode_;
}

Path Backend::ActivePath() const {
    return path_;
}

SubmitStats Backend::Submit(std::span<const std::uint32_t> dwords) {
    SubmitStats stats{};
    Mode requested = mode_;
    if (requested == Mode::Auto) {
        const char* env = std::getenv("ANYPS5_CPU_BACKEND");
        if (env != nullptr) {
            requested = ModeFromEnv();
            if (requested == Mode::Auto) {
                requested = Mode::Fast;
            }
        } else {
            requested = Mode::Fast;
        }
    }
    auto cpuBegin = std::chrono::steady_clock::now();
    if (decodeScratch_.size() < dwords.size()) {
        decodeScratch_.resize(dwords.size() > 0 ? dwords.size() : 1);
    }
    DecodeStats decodeStats{};
    auto decodeBegin = std::chrono::steady_clock::now();
    std::size_t count = DecodeRange(dwords, std::span<DecodedPacket>(decodeScratch_.data(), dwords.size() > 0 ? dwords.size() : 1), &decodeStats);
    std::span<const DecodedPacket> decoded(decodeScratch_.data(), count);
    auto decodeEnd = std::chrono::steady_clock::now();
    timer_.AddDecode(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(decodeEnd - decodeBegin).count()));
    timer_.AddPackets(decodeStats.packets);
    timer_.AddDraws(decodeStats.draws);
    timer_.AddDispatches(decodeStats.dispatches);
    stats.packets = decodeStats.packets;
    stats.draws = decodeStats.draws;
    stats.dispatches = decodeStats.dispatches;
    bool usable = CanUseFast(decoded) && decodeStats.truncated == 0;
    Mode used = requested;
    if (requested == Mode::Fast && !usable) {
        used = Mode::Legacy;
        stats.fellBack = true;
    }
    if (requested == Mode::Legacy) {
        used = Mode::Legacy;
    }
    stats.used = used;
    if (used == Mode::Legacy) {
        auto recordBegin = std::chrono::steady_clock::now();
        ExecCounters counters{};
        lastDigest_ = LegacyExecute(dwords, &counters);
        auto recordEnd = std::chrono::steady_clock::now();
        timer_.Add(Phase::CpuPrepare, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(decodeEnd - cpuBegin).count()));
        timer_.Add(Phase::Record, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(recordEnd - recordBegin).count()));
        timer_.AddSubmissions(1);
        stats.batches = decoded.size();
        return stats;
    }
    auto planBegin = std::chrono::steady_clock::now();
    PlanStats planStats{};
    batchScratch_.clear();
    {
        std::vector<Batch> planned = PlanBatches(decoded, &planStats);
        batchScratch_.assign(planned.begin(), planned.end());
    }
    std::span<const Batch> batches(batchScratch_.data(), batchScratch_.size());
    auto planEnd = std::chrono::steady_clock::now();
    timer_.AddPrepare(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(planEnd - planBegin).count()));
    stats.batches = planStats.batches;
    tick_ += 1;
    serial_ += 1;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    auto resourceBegin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const DecodedPacket& packet = decodeScratch_[i];
        if (packet.packetClass != PacketClass::Draw && packet.packetClass != PacketClass::Dispatch) {
            continue;
        }
        std::span<const std::uint32_t> words(dwords.data() + packet.offset, packet.words);
        ResourceKey key{};
        key.address = static_cast<std::uint64_t>(packet.offset) * 4ull;
        key.sizeBytes = static_cast<std::uint64_t>(packet.words) * 4ull;
        key.format = packet.header;
        key.layout = packet.opcode;
        key.version = 1;
        key.epoch = 1;
        key.writeSerial = 0;
        key.kind = static_cast<std::uint32_t>(packet.packetClass);
        key.extra = 0;
        LookupOutcome outcome = cache_.Lookup(key, words.data(), words.size());
        if (outcome.hit) {
            hits += 1;
        } else {
            misses += 1;
            cache_.Insert(key, words.data(), words.size(), 0);
        }
    }
    auto resourceEnd = std::chrono::steady_clock::now();
    timer_.AddResource(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(resourceEnd - resourceBegin).count()));
    stats.resourceHits = hits;
    stats.resourceMisses = misses;
    auto recordBegin = std::chrono::steady_clock::now();
    Executor executor;
    executor.ApplyBatches(dwords, decoded, batches);
    lastDigest_ = executor.Digest();
    auto recordEnd = std::chrono::steady_clock::now();
    timer_.Add(Phase::CpuPrepare, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(planBegin - cpuBegin).count() +
                                                             std::chrono::duration_cast<std::chrono::nanoseconds>(planEnd - planBegin).count() +
                                                             std::chrono::duration_cast<std::chrono::nanoseconds>(resourceEnd - resourceBegin).count()));
    timer_.Add(Phase::Record, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(recordEnd - recordBegin).count()));
    timer_.AddSubmissions(1);
    return stats;
}

PhaseSnapshot Backend::Snapshot() const {
    return timer_.Snapshot();
}

void Backend::ResetStats() {
    timer_.Reset();
    cache_.Clear();
    lastDigest_ = 0;
}

ResourceStats Backend::ResourceStatsSnapshot() const {
    return cache_.Stats();
}

std::uint64_t Backend::LastDigest() const {
    return lastDigest_;
}

} // namespace AgcDriver::CpuBackend
