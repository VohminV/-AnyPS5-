#ifndef CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_RDNAPROGRAM_HPP
#define CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_RDNAPROGRAM_HPP

#include "RdnaDecoder/RdnaInstruction.hpp"
#include <cstdint>
#include <vector>
#include <span>

namespace ShaderRecompiler {

struct RdnaProgram {
    std::span<const std::uint32_t> code;
    std::vector<RdnaInstruction> instructions;
    // All-zero words skipped while decoding (inter-function padding or embedded
    // data such as a zero jump-table offset). ISA 12.7 has no VOP2 opcode 0 and
    // bit 31 = 0 forces the VOP2 family, so a zero word is provably not an
    // instruction; each skipped word still advances the program counter by 4.
    std::uint32_t skippedZeroWords = 0;
};

[[nodiscard]] const RdnaInstruction* FindInstructionAtProgramCounter(const RdnaProgram& program, std::uint32_t programCounter);

}

#endif
