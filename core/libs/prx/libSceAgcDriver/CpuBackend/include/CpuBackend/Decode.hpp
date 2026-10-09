#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::CpuBackend {

enum class PacketClass : std::uint8_t {
    Unknown = 0,
    Filler = 1,
    Nop = 2,
    SetConfig = 3,
    SetContext = 4,
    SetSh = 5,
    SetUconfig = 6,
    SetQueue = 7,
    Draw = 8,
    Dispatch = 9,
    Sync = 10,
    Memory = 11,
    IndirectRegs = 12,
    Control = 13
};

struct DecodedPacket {
    std::uint32_t header = 0;
    std::uint32_t opcode = 0;
    std::size_t offset = 0;
    std::size_t words = 0;
    PacketClass packetClass = PacketClass::Unknown;
    bool predicated = false;
    bool touchesMemory = false;
    bool mergeable = false;
};

inline bool IsFiller(std::uint32_t header) {
    return (header >> 30u) == 2u;
}

inline std::size_t PacketWordCount(std::uint32_t header) {
    if (IsFiller(header)) {
        return 1u;
    }
    return static_cast<std::size_t>((header >> 16u) & 0x3fffu) + 2u;
}

inline std::uint32_t PacketOpcode(std::uint32_t header) {
    return (header >> 8u) & 0xffu;
}

PacketClass Classify(std::uint32_t opcode);
bool TouchesMemory(std::uint32_t opcode);
bool MergeableClass(PacketClass value);

struct DecodeStats {
    std::size_t packets = 0;
    std::size_t fillers = 0;
    std::size_t draws = 0;
    std::size_t dispatches = 0;
    std::size_t truncated = 0;
};

std::size_t DecodeRange(std::span<const std::uint32_t> dwords, std::span<DecodedPacket> out, DecodeStats* stats);
std::vector<DecodedPacket> DecodeAll(std::span<const std::uint32_t> dwords, DecodeStats* stats);
bool ValidateBounds(std::span<const std::uint32_t> dwords);

} // namespace AgcDriver::CpuBackend
