#include "CpuBackend/Decode.hpp"

namespace AgcDriver::CpuBackend {

PacketClass Classify(std::uint32_t opcode) {
    switch (opcode) {
        case 0x10:
            return PacketClass::Nop;
        case 0x68:
        case 0x11:
        case 0x13:
        case 0x26:
        case 0x2a:
        case 0x2f:
            return PacketClass::SetConfig;
        case 0x69:
        case 0x9f:
            return PacketClass::SetContext;
        case 0x76:
        case 0x63:
            return PacketClass::SetSh;
        case 0x79:
        case 0x7a:
        case 0x64:
            return PacketClass::SetUconfig;
        case 0x78:
            return PacketClass::SetQueue;
        case 0x27:
        case 0x2d:
        case 0x35:
        case 0x24:
        case 0x25:
        case 0x2c:
        case 0x38:
            return PacketClass::Draw;
        case 0x15:
        case 0x16:
        case 0x8d:
            return PacketClass::Dispatch;
        case 0x3c:
        case 0x93:
        case 0x58:
        case 0x42:
        case 0x43:
        case 0x47:
        case 0x48:
        case 0x49:
        case 0x39:
        case 0x45:
        case 0x46:
            return PacketClass::Sync;
        case 0x40:
        case 0x41:
        case 0x50:
        case 0x37:
        case 0x81:
        case 0x83:
            return PacketClass::Memory;
        case 0x12:
        case 0x28:
        case 0x20:
        case 0x22:
        case 0x33:
        case 0x3a:
        case 0x3f:
        case 0x59:
        case 0x84:
        case 0x85:
        case 0x86:
        case 0x88:
        case 0x8e:
            return PacketClass::Control;
        default:
            break;
    }
    if (opcode == 0x63 || opcode == 0x64 || opcode == 0x9f) {
        return PacketClass::IndirectRegs;
    }
    return PacketClass::Unknown;
}

bool TouchesMemory(std::uint32_t opcode) {
    PacketClass value = Classify(opcode);
    return value == PacketClass::Draw || value == PacketClass::Dispatch || value == PacketClass::Sync ||
           value == PacketClass::Memory || value == PacketClass::IndirectRegs;
}

bool MergeableClass(PacketClass value) {
    return value == PacketClass::SetConfig || value == PacketClass::SetContext || value == PacketClass::SetSh ||
           value == PacketClass::SetUconfig || value == PacketClass::SetQueue;
}

bool ValidateBounds(std::span<const std::uint32_t> dwords) {
    std::size_t cursor = 0;
    while (cursor < dwords.size()) {
        std::uint32_t header = dwords[cursor];
        if (IsFiller(header)) {
            cursor += 1;
            continue;
        }
        std::size_t words = PacketWordCount(header);
        if (words < 2 || words > 0x4002) {
            return false;
        }
        if (cursor + words > dwords.size()) {
            return false;
        }
        std::uint32_t opcode = PacketOpcode(header);
        if (Classify(opcode) == PacketClass::Unknown) {
            return false;
        }
        cursor += words;
    }
    return true;
}

std::size_t DecodeRange(std::span<const std::uint32_t> dwords, std::span<DecodedPacket> out, DecodeStats* stats) {
    std::size_t cursor = 0;
    std::size_t produced = 0;
    DecodeStats local{};
    while (cursor < dwords.size()) {
        std::uint32_t header = dwords[cursor];
        if (IsFiller(header)) {
            if (produced < out.size()) {
                DecodedPacket entry{};
                entry.header = header;
                entry.opcode = 0xFFFFFFFFu;
                entry.offset = cursor;
                entry.words = 1;
                entry.packetClass = PacketClass::Filler;
                entry.predicated = false;
                entry.touchesMemory = false;
                entry.mergeable = true;
                out[produced] = entry;
            }
            produced += 1;
            local.packets += 1;
            local.fillers += 1;
            cursor += 1;
            continue;
        }
        std::size_t words = PacketWordCount(header);
        if (words < 2 || cursor + words > dwords.size()) {
            local.truncated += 1;
            break;
        }
        std::uint32_t opcode = PacketOpcode(header);
        PacketClass kind = Classify(opcode);
        if (opcode == 0x10u && (header & 0xFEu) != 0u) {
            kind = PacketClass::Control;
        }
        if (kind == PacketClass::Unknown) {
            local.truncated += 1;
            break;
        }
        if (produced < out.size()) {
            DecodedPacket entry{};
            entry.header = header;
            entry.opcode = opcode;
            entry.offset = cursor;
            entry.words = words;
            entry.packetClass = kind;
            entry.predicated = (header & 1u) != 0u;
            entry.touchesMemory = TouchesMemory(opcode);
            entry.mergeable = MergeableClass(kind) && !entry.predicated;
            out[produced] = entry;
        }
        produced += 1;
        local.packets += 1;
        if (kind == PacketClass::Draw) {
            local.draws += 1;
        }
        if (kind == PacketClass::Dispatch) {
            local.dispatches += 1;
        }
        cursor += words;
    }
    if (stats) {
        *stats = local;
    }
    return produced;
}

std::vector<DecodedPacket> DecodeAll(std::span<const std::uint32_t> dwords, DecodeStats* stats) {
    std::vector<DecodedPacket> out;
    out.resize(dwords.size());
    DecodeStats local{};
    std::size_t count = DecodeRange(dwords, std::span<DecodedPacket>(out.data(), out.size()), &local);
    if (count < out.size()) {
        out.resize(count);
    }
    if (stats) {
        *stats = local;
    }
    return out;
}

} // namespace AgcDriver::CpuBackend
