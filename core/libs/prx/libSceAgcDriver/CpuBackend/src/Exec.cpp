#include "CpuBackend/Exec.hpp"
#include "CpuBackend/Simd.hpp"

namespace AgcDriver::CpuBackend {

void PhaseTimer::Reset() {
    snapshot_ = PhaseSnapshot{};
}

void PhaseTimer::Add(Phase phase, std::uint64_t ns) {
    if (phase == Phase::CpuPrepare) {
        snapshot_.cpuPrepareNs += ns;
    } else if (phase == Phase::Record) {
        snapshot_.recordNs += ns;
    } else {
        snapshot_.gpuWaitNs += ns;
    }
}

void PhaseTimer::AddDecode(std::uint64_t ns) {
    snapshot_.decodeNs += ns;
}

void PhaseTimer::AddResource(std::uint64_t ns) {
    snapshot_.resourceNs += ns;
}

void PhaseTimer::AddPrepare(std::uint64_t ns) {
    snapshot_.prepareNs += ns;
}

void PhaseTimer::AddSync(std::uint64_t ns) {
    snapshot_.syncNs += ns;
}

void PhaseTimer::AddPackets(std::uint64_t count) {
    snapshot_.packets += count;
}

void PhaseTimer::AddDraws(std::uint64_t count) {
    snapshot_.draws += count;
}

void PhaseTimer::AddDispatches(std::uint64_t count) {
    snapshot_.dispatches += count;
}

void PhaseTimer::AddSubmissions(std::uint64_t count) {
    snapshot_.submissions += count;
}

PhaseSnapshot PhaseTimer::Snapshot() const {
    return snapshot_;
}

std::vector<Batch> PlanBatches(std::span<const DecodedPacket> packets, PlanStats* stats) {
    std::vector<Batch> out;
    out.reserve(packets.size());
    PlanStats local{};
    std::size_t i = 0;
    while (i < packets.size()) {
        const DecodedPacket& first = packets[i];
        if (first.packetClass == PacketClass::Filler || first.packetClass == PacketClass::Nop) {
            std::size_t j = i + 1;
            while (j < packets.size() &&
                   (packets[j].packetClass == PacketClass::Filler || packets[j].packetClass == PacketClass::Nop) &&
                   !packets[j].predicated) {
                ++j;
            }
            Batch batch{};
            batch.kind = BatchKind::NopRun;
            batch.beginPacket = i;
            batch.packetCount = j - i;
            batch.beginDword = packets[i].offset;
            batch.dwordCount = 0;
            for (std::size_t k = i; k < j; ++k) {
                batch.dwordCount += packets[k].words;
            }
            out.push_back(batch);
            local.batches += 1;
            local.nopRuns += 1;
            local.coalescedPackets += (j - i > 1) ? (j - i - 1) : 0;
            i = j;
            continue;
        }
        if (first.mergeable && !first.predicated && !first.touchesMemory) {
            std::size_t j = i + 1;
            while (j < packets.size() && packets[j].mergeable && !packets[j].predicated && !packets[j].touchesMemory &&
                   packets[j].packetClass == first.packetClass) {
                ++j;
                if (j - i >= 32) {
                    break;
                }
            }
            if (j - i > 1) {
                Batch batch{};
                batch.kind = BatchKind::SetRun;
                batch.beginPacket = i;
                batch.packetCount = j - i;
                batch.beginDword = packets[i].offset;
                batch.dwordCount = 0;
                for (std::size_t k = i; k < j; ++k) {
                    batch.dwordCount += packets[k].words;
                }
                out.push_back(batch);
                local.batches += 1;
                local.setRuns += 1;
                local.coalescedPackets += j - i - 1;
                i = j;
                continue;
            }
        }
        Batch batch{};
        batch.kind = BatchKind::Single;
        batch.beginPacket = i;
        batch.packetCount = 1;
        batch.beginDword = first.offset;
        batch.dwordCount = first.words;
        out.push_back(batch);
        local.batches += 1;
        local.singles += 1;
        i += 1;
    }
    if (stats) {
        *stats = local;
    }
    return out;
}

void Executor::Reset() {
    digest_ = 1469598103934665603ull;
    counters_ = ExecCounters{};
    registerHash_ = 1469598103934665603ull;
}

void Executor::MixPacket(std::uint32_t header, std::span<const std::uint32_t> words) {
    std::uint64_t h = Hash64Scalar(words.data(), words.size(), static_cast<std::uint64_t>(header));
    digest_ ^= h + 0x9e3779b97f4a7c15ull + (digest_ << 6ull) + (digest_ >> 2ull);
    digest_ *= 1099511628211ull;
}

void Executor::MixRegister(std::uint32_t space, std::uint32_t offset, std::uint32_t value) {
    std::uint64_t packed = (static_cast<std::uint64_t>(space) << 48ull) | (static_cast<std::uint64_t>(offset) << 32ull) | value;
    registerHash_ ^= packed + 0x9e3779b97f4a7c15ull + (registerHash_ << 6ull) + (registerHash_ >> 2ull);
    registerHash_ *= 1099511628211ull;
    digest_ ^= packed + 0x9e3779b97f4a7c15ull + (digest_ << 6ull) + (digest_ >> 2ull);
    digest_ *= 1099511628211ull;
}

void Executor::ApplyPackets(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded) {
    for (const auto& packet : decoded) {
        std::span<const std::uint32_t> words(dwords.data() + packet.offset, packet.words);
        MixPacket(packet.header, words);
        counters_.dwords += packet.words;
        switch (packet.packetClass) {
            case PacketClass::Filler:
            case PacketClass::Nop:
                counters_.nops += 1;
                break;
            case PacketClass::SetConfig:
                counters_.sets += 1;
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(0, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            case PacketClass::SetContext:
                counters_.sets += 1;
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(1, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            case PacketClass::SetSh:
                counters_.sets += 1;
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(2, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            case PacketClass::SetUconfig:
                counters_.sets += 1;
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(3, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            case PacketClass::SetQueue:
                counters_.sets += 1;
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(4, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            case PacketClass::Draw:
                counters_.draws += 1;
                break;
            case PacketClass::Dispatch:
                counters_.dispatches += 1;
                break;
            case PacketClass::Sync:
                counters_.syncs += 1;
                break;
            case PacketClass::Memory:
                counters_.memories += 1;
                break;
            default:
                counters_.controls += 1;
                break;
        }
    }
}

void Executor::ApplyBatches(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded, std::span<const Batch> batches) {
    for (const auto& batch : batches) {
        if (batch.kind == BatchKind::NopRun) {
            for (std::size_t p = 0; p < batch.packetCount; ++p) {
                const DecodedPacket& packet = decoded[batch.beginPacket + p];
                std::span<const std::uint32_t> words(dwords.data() + packet.offset, packet.words);
                MixPacket(packet.header, words);
                counters_.dwords += packet.words;
                counters_.nops += 1;
            }
            continue;
        }
        if (batch.kind == BatchKind::SetRun) {
            for (std::size_t p = 0; p < batch.packetCount; ++p) {
                const DecodedPacket& packet = decoded[batch.beginPacket + p];
                std::span<const std::uint32_t> words(dwords.data() + packet.offset, packet.words);
                MixPacket(packet.header, words);
                counters_.dwords += packet.words;
                counters_.sets += 1;
                std::uint32_t space = 0;
                if (packet.packetClass == PacketClass::SetContext) {
                    space = 1;
                } else if (packet.packetClass == PacketClass::SetSh) {
                    space = 2;
                } else if (packet.packetClass == PacketClass::SetUconfig) {
                    space = 3;
                } else if (packet.packetClass == PacketClass::SetQueue) {
                    space = 4;
                }
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(space, static_cast<std::uint32_t>(w), words[w]);
                }
            }
            continue;
        }
        const DecodedPacket& packet = decoded[batch.beginPacket];
        std::span<const std::uint32_t> words(dwords.data() + packet.offset, packet.words);
        MixPacket(packet.header, words);
        counters_.dwords += packet.words;
        switch (packet.packetClass) {
            case PacketClass::Filler:
            case PacketClass::Nop:
                counters_.nops += 1;
                break;
            case PacketClass::SetConfig:
            case PacketClass::SetContext:
            case PacketClass::SetSh:
            case PacketClass::SetUconfig:
            case PacketClass::SetQueue: {
                counters_.sets += 1;
                std::uint32_t space = 0;
                if (packet.packetClass == PacketClass::SetContext) {
                    space = 1;
                } else if (packet.packetClass == PacketClass::SetSh) {
                    space = 2;
                } else if (packet.packetClass == PacketClass::SetUconfig) {
                    space = 3;
                } else if (packet.packetClass == PacketClass::SetQueue) {
                    space = 4;
                }
                for (std::size_t w = 2; w < words.size(); ++w) {
                    MixRegister(space, static_cast<std::uint32_t>(w), words[w]);
                }
                break;
            }
            case PacketClass::Draw:
                counters_.draws += 1;
                break;
            case PacketClass::Dispatch:
                counters_.dispatches += 1;
                break;
            case PacketClass::Sync:
                counters_.syncs += 1;
                break;
            case PacketClass::Memory:
                counters_.memories += 1;
                break;
            default:
                counters_.controls += 1;
                break;
        }
    }
}

std::uint64_t Executor::Digest() const {
    return digest_;
}

ExecCounters Executor::Counters() const {
    return counters_;
}

std::uint64_t Executor::RegisterStateHash() const {
    return registerHash_;
}

std::uint64_t LegacyExecute(std::span<const std::uint32_t> dwords, ExecCounters* counters) {
    std::uint64_t digest = 1469598103934665603ull;
    std::uint64_t regHash = 1469598103934665603ull;
    ExecCounters local{};
    std::size_t cursor = 0;
    while (cursor < dwords.size()) {
        std::uint32_t header = dwords[cursor];
        std::size_t words = PacketWordCount(header);
        if (IsFiller(header)) {
            words = 1;
        }
        if (cursor + words > dwords.size()) {
            break;
        }
        std::span<const std::uint32_t> packet(dwords.data() + cursor, words);
        std::uint64_t h = Hash64Scalar(packet.data(), packet.size(), static_cast<std::uint64_t>(header));
        digest ^= h + 0x9e3779b97f4a7c15ull + (digest << 6ull) + (digest >> 2ull);
        digest *= 1099511628211ull;
        local.dwords += words;
        if (IsFiller(header)) {
            local.nops += 1;
            cursor += 1;
            continue;
        }
        std::uint32_t opcode = PacketOpcode(header);
        PacketClass kind = Classify(opcode);
        switch (kind) {
            case PacketClass::Nop:
                local.nops += 1;
                break;
            case PacketClass::SetConfig:
            case PacketClass::SetContext:
            case PacketClass::SetSh:
            case PacketClass::SetUconfig:
            case PacketClass::SetQueue: {
                local.sets += 1;
                std::uint32_t space = 0;
                if (kind == PacketClass::SetContext) {
                    space = 1;
                } else if (kind == PacketClass::SetSh) {
                    space = 2;
                } else if (kind == PacketClass::SetUconfig) {
                    space = 3;
                } else if (kind == PacketClass::SetQueue) {
                    space = 4;
                }
                for (std::size_t w = 2; w < packet.size(); ++w) {
                    std::uint64_t packed = (static_cast<std::uint64_t>(space) << 48ull) |
                                           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(w)) << 32ull) | packet[w];
                    regHash ^= packed + 0x9e3779b97f4a7c15ull + (regHash << 6ull) + (regHash >> 2ull);
                    regHash *= 1099511628211ull;
                    digest ^= packed + 0x9e3779b97f4a7c15ull + (digest << 6ull) + (digest >> 2ull);
                    digest *= 1099511628211ull;
                }
                break;
            }
            case PacketClass::Draw:
                local.draws += 1;
                break;
            case PacketClass::Dispatch:
                local.dispatches += 1;
                break;
            case PacketClass::Sync:
                local.syncs += 1;
                break;
            case PacketClass::Memory:
                local.memories += 1;
                break;
            default:
                local.controls += 1;
                break;
        }
        cursor += words;
    }
    if (counters) {
        *counters = local;
    }
    return digest;
}

bool SameObservable(std::span<const std::uint32_t> dwords, std::span<const DecodedPacket> decoded, std::span<const Batch> batches) {
    Executor fast;
    fast.ApplyBatches(dwords, decoded, batches);
    ExecCounters legacyCounters{};
    std::uint64_t legacyDigest = LegacyExecute(dwords, &legacyCounters);
    ExecCounters fastCounters = fast.Counters();
    return legacyDigest == fast.Digest() && legacyCounters.draws == fastCounters.draws &&
           legacyCounters.dispatches == fastCounters.dispatches && legacyCounters.sets == fastCounters.sets &&
           legacyCounters.nops == fastCounters.nops && legacyCounters.syncs == fastCounters.syncs &&
           legacyCounters.memories == fastCounters.memories;
}

} // namespace AgcDriver::CpuBackend
