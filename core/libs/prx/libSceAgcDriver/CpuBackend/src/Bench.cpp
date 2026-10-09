#include "CpuBackend/Bench.hpp"
#include "CpuBackend/Decode.hpp"
#include "CpuBackend/Exec.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>

namespace AgcDriver::CpuBackend {

namespace {
std::uint64_t NextRandom(std::uint64_t& state) {
    state ^= state << 13ull;
    state ^= state >> 7ull;
    state ^= state << 17ull;
    return state;
}

void EmitPacket(std::vector<std::uint32_t>& out, std::uint32_t opcode, std::span<const std::uint32_t> payload) {
    std::uint32_t count = static_cast<std::uint32_t>(payload.size());
    std::uint32_t header = 0xC0000000u | ((count << 16u)) | (opcode << 8u);
    if (count == 0) {
        header = 0xC0000000u | (opcode << 8u);
        out.push_back(header);
        out.push_back(0);
        return;
    }
    header = 0xC0000000u | (((count - 1u) & 0x3FFFu) << 16u) | (opcode << 8u);
    out.push_back(header);
    for (auto v : payload) {
        out.push_back(v);
    }
}

void EmitFiller(std::vector<std::uint32_t>& out, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(0x80000000u);
    }
}

std::vector<std::uint32_t> BuildDrawHeavy(std::uint64_t seed) {
    std::vector<std::uint32_t> out;
    out.reserve(8192);
    std::uint64_t state = seed;
    for (int i = 0; i < 120; ++i) {
        std::uint32_t base = static_cast<std::uint32_t>(NextRandom(state) & 0xFFFFu);
        std::uint32_t cfg[2] = {0x242u, base};
        EmitPacket(out, 0x79, std::span<const std::uint32_t>(cfg, 2));
        std::uint32_t ctx[2] = {0x318u, static_cast<std::uint32_t>(NextRandom(state) & 0xFFu)};
        EmitPacket(out, 0x69, std::span<const std::uint32_t>(ctx, 2));
        std::uint32_t sh[2] = {0x8Cu, static_cast<std::uint32_t>(NextRandom(state))};
        EmitPacket(out, 0x76, std::span<const std::uint32_t>(sh, 2));
        std::uint32_t draw[5] = {3u, 0x1000u + static_cast<std::uint32_t>(i * 64u), 0u, 3u, 0u};
        EmitPacket(out, 0x27, std::span<const std::uint32_t>(draw, 5));
        if (i % 8 == 7) {
            std::uint32_t rel[4] = {0u, 0x100u, 1u, 0u};
            EmitPacket(out, 0x49, std::span<const std::uint32_t>(rel, 4));
        }
        if (i % 16 == 15) {
            EmitFiller(out, 2);
        }
    }
    return out;
}

std::vector<std::uint32_t> BuildDispatchHeavy(std::uint64_t seed) {
    std::vector<std::uint32_t> out;
    out.reserve(8192);
    std::uint64_t state = seed;
    for (int i = 0; i < 160; ++i) {
        std::uint32_t cfg[2] = {0x243u, static_cast<std::uint32_t>(NextRandom(state) & 0x3u)};
        EmitPacket(out, 0x79, std::span<const std::uint32_t>(cfg, 2));
        std::uint32_t disp[4] = {8u + static_cast<std::uint32_t>(i % 4u), 8u, 1u, 0u};
        EmitPacket(out, 0x15, std::span<const std::uint32_t>(disp, 4));
        if (i % 6 == 5) {
            std::uint32_t acq[6] = {0u, 0xFFFFFFFFu, 0u, 0u, 0u, 0u};
            EmitPacket(out, 0x58, std::span<const std::uint32_t>(acq, 6));
        }
        if (i % 10 == 9) {
            EmitFiller(out, 1);
        }
    }
    return out;
}

std::vector<std::uint32_t> BuildMixed(std::uint64_t seed) {
    std::vector<std::uint32_t> out;
    out.reserve(16384);
    std::uint64_t state = seed;
    for (int i = 0; i < 90; ++i) {
        std::uint32_t cfg[2] = {0x242u, static_cast<std::uint32_t>(NextRandom(state) & 0xFFu)};
        EmitPacket(out, 0x79, std::span<const std::uint32_t>(cfg, 2));
        std::uint32_t draw[5] = {3u, 0x2000u + static_cast<std::uint32_t>(i * 32u), 0u, 3u, 0u};
        EmitPacket(out, 0x27, std::span<const std::uint32_t>(draw, 5));
        std::uint32_t disp[4] = {4u, 4u, 1u, 0u};
        EmitPacket(out, 0x15, std::span<const std::uint32_t>(disp, 4));
        std::uint32_t wait[5] = {0u, 1u, 0u, 0u, 0u};
        EmitPacket(out, 0x3C, std::span<const std::uint32_t>(wait, 5));
        std::uint32_t rel[4] = {0u, 0x200u, 2u, 0u};
        EmitPacket(out, 0x49, std::span<const std::uint32_t>(rel, 4));
        if (i % 4 == 3) {
            EmitFiller(out, 3);
        }
    }
    return out;
}

std::vector<std::uint32_t> BuildStateChurn(std::uint64_t seed) {
    std::vector<std::uint32_t> out;
    out.reserve(16384);
    std::uint64_t state = seed;
    (void)state;
    for (int i = 0; i < 400; ++i) {
        std::uint32_t v = static_cast<std::uint32_t>(i & 0xFFu);
        std::uint32_t a[2] = {0x100u + static_cast<std::uint32_t>(i % 16u), v};
        EmitPacket(out, 0x68, std::span<const std::uint32_t>(a, 2));
        std::uint32_t b[2] = {0x200u + static_cast<std::uint32_t>(i % 16u), v + 1u};
        EmitPacket(out, 0x69, std::span<const std::uint32_t>(b, 2));
        std::uint32_t c[2] = {0x8Cu + static_cast<std::uint32_t>(i % 8u), v + 2u};
        EmitPacket(out, 0x76, std::span<const std::uint32_t>(c, 2));
    }
    std::uint32_t draw[5] = {3u, 0x4000u, 0u, 3u, 0u};
    for (int i = 0; i < 40; ++i) {
        EmitPacket(out, 0x27, std::span<const std::uint32_t>(draw, 5));
    }
    return out;
}

std::vector<std::uint32_t> BuildSyncHeavy() {
    std::vector<std::uint32_t> out;
    out.reserve(8192);
    for (int i = 0; i < 120; ++i) {
        std::uint32_t wait[5] = {0u, 1u, 0u, 0u, 0u};
        EmitPacket(out, 0x3C, std::span<const std::uint32_t>(wait, 5));
        std::uint32_t rel[4] = {0u, 0x300u + static_cast<std::uint32_t>(i * 8u), 3u, 0u};
        EmitPacket(out, 0x49, std::span<const std::uint32_t>(rel, 4));
        std::uint32_t acq[6] = {0u, 0xFFFFFFFFu, 0u, 0u, 0u, 0u};
        EmitPacket(out, 0x58, std::span<const std::uint32_t>(acq, 6));
        if (i % 3 == 2) {
            EmitFiller(out, 2);
        }
    }
    return out;
}
} // namespace

std::vector<StreamCase> MakeStreamCases(std::uint32_t seed) {
    std::vector<StreamCase> out;
    StreamCase a{};
    a.name = "draw_heavy";
    a.dwords = BuildDrawHeavy(static_cast<std::uint64_t>(seed) * 2685821657736338717ull + 1ull);
    out.push_back(std::move(a));
    StreamCase b{};
    b.name = "dispatch_heavy";
    b.dwords = BuildDispatchHeavy(static_cast<std::uint64_t>(seed) * 2685821657736338717ull + 2ull);
    out.push_back(std::move(b));
    StreamCase c{};
    c.name = "mixed_submit";
    c.dwords = BuildMixed(static_cast<std::uint64_t>(seed) * 2685821657736338717ull + 3ull);
    out.push_back(std::move(c));
    StreamCase d{};
    d.name = "state_churn";
    d.dwords = BuildStateChurn(static_cast<std::uint64_t>(seed) * 2685821657736338717ull + 4ull);
    out.push_back(std::move(d));
    StreamCase e{};
    e.name = "sync_heavy";
    e.dwords = BuildSyncHeavy();
    out.push_back(std::move(e));
    return out;
}

BenchReport RunBench(const std::vector<StreamCase>& cases, std::size_t repeats) {
    BenchReport report{};
    if (repeats == 0) {
        repeats = 1;
    }
    for (const auto& item : cases) {
        CaseResult result{};
        result.name = item.name;
        result.dwords = item.dwords.size();
        std::span<const std::uint32_t> words(item.dwords.data(), item.dwords.size());
        DecodeStats stats{};
        auto decoded = DecodeAll(words, &stats);
        result.packets = stats.packets;
        ExecCounters legacyCounters{};
        std::uint64_t legacyDigest = 0;
        auto legacyBegin = std::chrono::steady_clock::now();
        for (std::size_t r = 0; r < repeats; ++r) {
            legacyDigest = LegacyExecute(words, r + 1 == repeats ? &legacyCounters : nullptr);
        }
        auto legacyEnd = std::chrono::steady_clock::now();
        double legacyUs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(legacyEnd - legacyBegin).count()) / 1000.0 / static_cast<double>(repeats);
        Backend backend;
        backend.SetMode(Mode::Fast);
        std::uint64_t fastDigest = 0;
        SubmitStats lastStats{};
        auto fastBegin = std::chrono::steady_clock::now();
        for (std::size_t r = 0; r < repeats; ++r) {
            lastStats = backend.Submit(words);
            fastDigest = backend.LastDigest();
        }
        auto fastEnd = std::chrono::steady_clock::now();
        double fastUs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(fastEnd - fastBegin).count()) / 1000.0 / static_cast<double>(repeats);
        result.legacyUs = legacyUs;
        result.fastUs = fastUs;
        result.legacyDigest = legacyDigest;
        result.fastDigest = fastDigest;
        result.identical = (legacyDigest == fastDigest);
        result.legacyDraws = legacyCounters.draws;
        result.fastDraws = lastStats.draws;
        result.fastHits = lastStats.resourceHits;
        result.fastMisses = lastStats.resourceMisses;
        if (stats.packets != 0) {
            result.perCommandNsLegacy = legacyUs * 1000.0 / static_cast<double>(stats.packets);
            result.perCommandNsFast = fastUs * 1000.0 / static_cast<double>(stats.packets);
        }
        if (fastUs > 0.0) {
            result.speedup = legacyUs / fastUs;
        }
        PhaseSnapshot fastSnap = backend.Snapshot();
        fastSnap.submissions = repeats;
        result.fastPhases = fastSnap;
        report.cases.push_back(result);
        report.totalLegacyUs += legacyUs;
        report.totalFastUs += fastUs;
        if (!result.identical) {
            report.allIdentical = false;
        }
    }
    if (report.totalFastUs > 0.0) {
        report.totalSpeedup = report.totalLegacyUs / report.totalFastUs;
    }
    return report;
}

std::string FormatReport(const BenchReport& report) {
    std::string out;
    char line[512];
    std::snprintf(line, sizeof(line), "cases=%zu legacy=%.1fus fast=%.1fus speedup=%.2fx identical=%d\n", report.cases.size(), report.totalLegacyUs, report.totalFastUs, report.totalSpeedup, report.allIdentical ? 1 : 0);
    out += line;
    for (const auto& item : report.cases) {
        std::snprintf(line, sizeof(line), "%s dwords=%zu packets=%zu legacy=%.1fus fast=%.1fus per_cmd_ns=%.1f/%.1f speedup=%.2f identical=%d draws=%llu/%llu hits=%llu misses=%llu\n", item.name.c_str(), item.dwords, item.packets, item.legacyUs, item.fastUs, item.perCommandNsLegacy, item.perCommandNsFast, item.speedup, item.identical ? 1 : 0, static_cast<unsigned long long>(item.legacyDraws), static_cast<unsigned long long>(item.fastDraws), static_cast<unsigned long long>(item.fastHits), static_cast<unsigned long long>(item.fastMisses));
        out += line;
    }
    return out;
}

} // namespace AgcDriver::CpuBackend
