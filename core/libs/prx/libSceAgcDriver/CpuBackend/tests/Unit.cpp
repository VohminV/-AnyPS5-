#include "CpuBackend/Backend.hpp"
#include "CpuBackend/Bench.hpp"
#include "CpuBackend/Decode.hpp"
#include "CpuBackend/Exec.hpp"
#include "CpuBackend/Resources.hpp"
#include "CpuBackend/Schedule.hpp"
#include "CpuBackend/Simd.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* text) {
    if (!value) {
        throw std::runtime_error(text);
    }
}

std::vector<std::uint32_t> Packet(std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    std::vector<std::uint32_t> out;
    std::uint32_t count = static_cast<std::uint32_t>(payload.size());
    std::uint32_t header = 0xC0000000u | (((count - 1u) & 0x3FFFu) << 16u) | (opcode << 8u);
    out.push_back(header);
    for (auto v : payload) {
        out.push_back(v);
    }
    return out;
}

void Append(std::vector<std::uint32_t>& out, const std::vector<std::uint32_t>& part) {
    for (auto v : part) {
        out.push_back(v);
    }
}

void TestSimd() {
    AgcDriver::CpuBackend::Features features = AgcDriver::CpuBackend::Probe();
    AgcDriver::CpuBackend::Path selected = AgcDriver::CpuBackend::Select(features);
    Require(AgcDriver::CpuBackend::Name(selected) != nullptr, "simd name");
    std::vector<std::uint32_t> data(256);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint32_t>(i * 2654435761u + 97u);
    }
    std::uint64_t a = AgcDriver::CpuBackend::Hash64Scalar(data.data(), data.size(), 7u);
    std::uint64_t b = AgcDriver::CpuBackend::Hash64Sse(data.data(), data.size(), 7u);
    std::uint64_t c = AgcDriver::CpuBackend::Hash64Avx2(data.data(), data.size(), 7u);
    Require(a == b, "sse hash differs");
    Require(a == c, "avx2 hash differs");
    Require(AgcDriver::CpuBackend::EqualScalar(data.data(), data.data(), data.size()), "equal self");
    Require(AgcDriver::CpuBackend::EqualSse(data.data(), data.data(), data.size()), "sse equal self");
    Require(AgcDriver::CpuBackend::EqualAvx2(data.data(), data.data(), data.size()), "avx2 equal self");
    std::vector<std::uint32_t> other = data;
    other[100] ^= 1u;
    Require(!AgcDriver::CpuBackend::EqualScalar(data.data(), other.data(), data.size()), "scalar miss");
    Require(!AgcDriver::CpuBackend::EqualSse(data.data(), other.data(), data.size()), "sse miss");
    Require(!AgcDriver::CpuBackend::EqualAvx2(data.data(), other.data(), data.size()), "avx2 miss");
    std::vector<std::uint32_t> copy(data.size());
    AgcDriver::CpuBackend::CopySse(copy.data(), data.data(), data.size());
    Require(AgcDriver::CpuBackend::EqualScalar(copy.data(), data.data(), data.size()), "sse copy");
    AgcDriver::CpuBackend::CopyAvx2(copy.data(), data.data(), data.size());
    Require(AgcDriver::CpuBackend::EqualScalar(copy.data(), data.data(), data.size()), "avx2 copy");
    Require(AgcDriver::CpuBackend::EqualDispatch(data.data(), data.data(), data.size(), AgcDriver::CpuBackend::Path::Scalar), "dispatch scalar");
    Require(AgcDriver::CpuBackend::EqualDispatch(data.data(), data.data(), data.size(), AgcDriver::CpuBackend::Path::Sse), "dispatch sse");
    Require(AgcDriver::CpuBackend::EqualDispatch(data.data(), data.data(), data.size(), AgcDriver::CpuBackend::Path::Avx2), "dispatch avx2");
    std::uint64_t d = AgcDriver::CpuBackend::Hash64Dispatch(data.data(), data.size(), 7u, AgcDriver::CpuBackend::Path::Sse);
    Require(a == d, "dispatch hash");
}

void TestDecode() {
    Require(AgcDriver::CpuBackend::IsFiller(0x80000000u), "filler");
    Require(!AgcDriver::CpuBackend::IsFiller(0xC0001000u), "not filler");
    Require(AgcDriver::CpuBackend::PacketWordCount(0x80000000u) == 1, "filler words");
    Require(AgcDriver::CpuBackend::Classify(0x27) == AgcDriver::CpuBackend::PacketClass::Draw, "draw class");
    Require(AgcDriver::CpuBackend::Classify(0x15) == AgcDriver::CpuBackend::PacketClass::Dispatch, "dispatch class");
    Require(AgcDriver::CpuBackend::Classify(0x10) == AgcDriver::CpuBackend::PacketClass::Nop, "nop class");
    Require(AgcDriver::CpuBackend::Classify(0xFF) == AgcDriver::CpuBackend::PacketClass::Unknown, "unknown class");
    std::vector<std::uint32_t> stream;
    stream.push_back(0x80000000u);
    Append(stream, Packet(0x68, {0x100u, 1u}));
    Append(stream, Packet(0x27, {3u, 0x1000u, 0u, 3u, 0u}));
    Append(stream, Packet(0x15, {8u, 8u, 1u, 0u}));
    Require(AgcDriver::CpuBackend::ValidateBounds(stream), "bounds");
    std::vector<AgcDriver::CpuBackend::DecodedPacket> out(stream.size());
    AgcDriver::CpuBackend::DecodeStats stats{};
    std::size_t count = AgcDriver::CpuBackend::DecodeRange(stream, std::span<AgcDriver::CpuBackend::DecodedPacket>(out.data(), out.size()), &stats);
    Require(count == 4, "decode count");
    Require(stats.draws == 1, "draw stat");
    Require(stats.dispatches == 1, "dispatch stat");
    Require(stats.fillers == 1, "filler stat");
    std::vector<std::uint32_t> bad{0xC03F2800u};
    Require(!AgcDriver::CpuBackend::ValidateBounds(bad), "bad bounds");
    Require(AgcDriver::CpuBackend::Classify(0x3Fu) == AgcDriver::CpuBackend::PacketClass::Control, "indirect buffer class");
    std::vector<std::uint32_t> customFlip{0xC004105Cu, 0u, 0u, 0u, 0u, 0u};
    auto customDecoded = AgcDriver::CpuBackend::DecodeAll(std::span<const std::uint32_t>(customFlip.data(), customFlip.size()), nullptr);
    Require(customDecoded.size() == 1, "custom count");
    Require(customDecoded[0].packetClass == AgcDriver::CpuBackend::PacketClass::Control, "custom flip class");
    std::vector<std::uint32_t> plainNop{0xC0001000u, 0u};
    auto nopDecoded = AgcDriver::CpuBackend::DecodeAll(std::span<const std::uint32_t>(plainNop.data(), plainNop.size()), nullptr);
    Require(nopDecoded.size() == 1, "nop count");
    Require(nopDecoded[0].packetClass == AgcDriver::CpuBackend::PacketClass::Nop, "plain nop class");
}

void TestResources() {
    AgcDriver::CpuBackend::ResourceCache cache;
    AgcDriver::CpuBackend::ResourceKey key{};
    key.address = 0x1000;
    key.sizeBytes = 64;
    key.format = 1;
    key.layout = 2;
    key.version = 1;
    key.epoch = 1;
    key.writeSerial = 0;
    key.kind = 8;
    std::vector<std::uint32_t> content{1u, 2u, 3u, 4u};
    auto miss = cache.Lookup(key, content.data(), content.size());
    Require(!miss.hit, "first lookup miss");
    std::uint64_t h = AgcDriver::CpuBackend::ContentHash(content.data(), content.size(), 0);
    Require(cache.Insert(key, content.data(), content.size(), h), "insert");
    auto hit = cache.Lookup(key, content.data(), content.size());
    Require(hit.hit, "second lookup hit");
    std::vector<std::uint32_t> changed{1u, 2u, 3u, 5u};
    auto changedLookup = cache.Lookup(key, changed.data(), changed.size());
    Require(!changedLookup.hit, "changed content miss");
    AgcDriver::CpuBackend::ResourceKey other = key;
    other.version = 2;
    auto versionLookup = cache.Lookup(other, content.data(), content.size());
    Require(!versionLookup.hit, "version miss");
    Require(cache.LiveEntries() == 1, "live entries");
    Require(cache.InvalidateRange(0x1000, 64) == 1, "invalidate");
    auto afterInvalid = cache.Lookup(key, content.data(), content.size());
    Require(!afterInvalid.hit, "invalidated miss");
    AgcDriver::CpuBackend::ResourceStats st = cache.Stats();
    Require(st.hits == 1, "hit stat");
    Require(st.misses >= 3, "miss stat");
}

void TestExec() {
    std::vector<std::uint32_t> stream;
    Append(stream, Packet(0x68, {0x100u, 1u}));
    Append(stream, Packet(0x68, {0x101u, 2u}));
    Append(stream, Packet(0x68, {0x102u, 3u}));
    stream.push_back(0x80000000u);
    stream.push_back(0x80000000u);
    Append(stream, Packet(0x27, {3u, 0x1000u, 0u, 3u, 0u}));
    Append(stream, Packet(0x15, {8u, 8u, 1u, 0u}));
    auto decoded = AgcDriver::CpuBackend::DecodeAll(std::span<const std::uint32_t>(stream.data(), stream.size()), nullptr);
    Require(decoded.size() == 7, "exec decode");
    AgcDriver::CpuBackend::PlanStats planStats{};
    auto batches = AgcDriver::CpuBackend::PlanBatches(decoded, &planStats);
    Require(planStats.setRuns == 1, "set run");
    Require(planStats.nopRuns == 1, "nop run");
    Require(AgcDriver::CpuBackend::SameObservable(std::span<const std::uint32_t>(stream.data(), stream.size()), std::span<const AgcDriver::CpuBackend::DecodedPacket>(decoded.data(), decoded.size()), std::span<const AgcDriver::CpuBackend::Batch>(batches.data(), batches.size())), "observable");
    AgcDriver::CpuBackend::ExecCounters legacyCounters{};
    std::uint64_t legacy = AgcDriver::CpuBackend::LegacyExecute(std::span<const std::uint32_t>(stream.data(), stream.size()), &legacyCounters);
    AgcDriver::CpuBackend::Executor executor;
    executor.ApplyBatches(std::span<const std::uint32_t>(stream.data(), stream.size()), std::span<const AgcDriver::CpuBackend::DecodedPacket>(decoded.data(), decoded.size()), std::span<const AgcDriver::CpuBackend::Batch>(batches.data(), batches.size()));
    Require(legacy == executor.Digest(), "digest");
    Require(legacyCounters.draws == 1, "draw count");
    Require(legacyCounters.dispatches == 1, "dispatch count");
}

void TestSchedule() {
    Require(AgcDriver::CpuBackend::RangesIndependent(0, 10, 10, 20), "adjacent independent");
    Require(!AgcDriver::CpuBackend::RangesIndependent(0, 11, 10, 20), "overlap");
    auto parts = AgcDriver::CpuBackend::SplitRange(10, 3);
    Require(parts.size() == 3, "split count");
    Require(parts[0].first == 0 && parts[2].second == 10, "split cover");
    AgcDriver::CpuBackend::ThreadPool pool(4);
    Require(pool.Threads() == 4, "threads");
    std::vector<int> seen(100, 0);
    pool.ForEach(100, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            seen[i] = static_cast<int>(i * 2);
        }
    });
    for (std::size_t i = 0; i < 100; ++i) {
        Require(seen[i] == static_cast<int>(i * 2), "parallel work");
    }
    Require(AgcDriver::CpuBackend::SuggestThreads() >= 1, "suggest");
}

void TestBackend() {
    AgcDriver::CpuBackend::Backend backend;
    backend.SetMode(AgcDriver::CpuBackend::Mode::Fast);
    Require(backend.GetMode() == AgcDriver::CpuBackend::Mode::Fast, "mode");
    std::vector<std::uint32_t> stream;
    Append(stream, Packet(0x68, {0x100u, 9u}));
    Append(stream, Packet(0x27, {3u, 0x2000u, 0u, 3u, 0u}));
    Append(stream, Packet(0x15, {4u, 4u, 1u, 0u}));
    std::span<const std::uint32_t> words(stream.data(), stream.size());
    auto first = backend.Submit(words);
    std::uint64_t digestFast = backend.LastDigest();
    Require(!first.fellBack, "no fallback");
    Require(first.used == AgcDriver::CpuBackend::Mode::Fast, "used fast");
    auto second = backend.Submit(words);
    Require(second.resourceHits > 0, "repeat hits");
    AgcDriver::CpuBackend::Backend legacy;
    legacy.SetMode(AgcDriver::CpuBackend::Mode::Legacy);
    legacy.Submit(words);
    Require(legacy.LastDigest() == digestFast, "legacy fast identical");
    std::vector<std::uint32_t> unknown{0xC000FF00u, 0u};
    AgcDriver::CpuBackend::Backend autoBackend;
    autoBackend.SetMode(AgcDriver::CpuBackend::Mode::Fast);
    auto outcome = autoBackend.Submit(std::span<const std::uint32_t>(unknown.data(), unknown.size()));
    Require(outcome.fellBack, "unknown fallback");
    Require(outcome.used == AgcDriver::CpuBackend::Mode::Legacy, "fallback used");
    Require(AgcDriver::CpuBackend::CanUseFast(std::span<const AgcDriver::CpuBackend::DecodedPacket>()) , "empty fast");
}
} // namespace

int main() {
    TestSimd();
    TestDecode();
    TestResources();
    TestExec();
    TestSchedule();
    TestBackend();
    std::puts("cpu_backend_unit ok");
    return 0;
}
