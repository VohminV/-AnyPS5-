#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;

void Require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "zero-word: %s\n", message);
        throw std::runtime_error(message);
    }
}

void CheckZeroWords() {
    // ISA 12.7 (gpu_research/amd-rdna2-isa.pdf) has no VOP2 opcode 0, and bit
    // 31 = 0 forces the VOP2 family, so an all-zero word is provably not an
    // instruction (padding or embedded data). The program decoder skips such
    // words one at a time, keeping later program counters aligned.
    // SOPP opcode 0 is s_nop, opcode 1 is s_endpgm.
    constexpr std::uint32_t filler = 0x80000000u | (0x7fu << 23u) | (0u << 16u);
    constexpr std::uint32_t endpgm = 0x80000000u | (0x7fu << 23u) | (1u << 16u);
    {
        const std::array<std::uint32_t, 4> words{filler, 0u, 0u, endpgm};
        const RdnaProgram program = RdnaInstructionDecoder{}.Decode(words);
        Require(program.instructions.size() == 2u, "interior zeros hide instructions");
        Require(program.instructions[0].programCounter == 0u, "first instruction moved");
        Require(program.instructions[1].programCounter == 12u, "program counter past zeros is wrong");
        Require(program.skippedZeroWords == 2u, "skipped zero count is wrong");
    }
    {
        // Trailing zeros are still trimmed, not counted as skipped.
        const std::array<std::uint32_t, 3> words{endpgm, 0u, 0u};
        const RdnaProgram program = RdnaInstructionDecoder{}.Decode(words);
        Require(program.instructions.size() == 1u, "trailing zeros break the trim");
        Require(program.skippedZeroWords == 0u, "trimmed zeros counted as skipped");
    }
    {
        // An all-zero span still fails as before (nothing to decode).
        const std::array<std::uint32_t, 3> words{0u, 0u, 0u};
        try {
            static_cast<void>(RdnaInstructionDecoder{}.Decode(words));
        } catch (const std::out_of_range&) {
            return;
        }
        Require(false, "all-zero span decodes");
    }
}

int main() {
    try {
        CheckZeroWords();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "zero-word padding: %s\n", error.what());
        return 1;
    }
    struct Case {
        const char* name;
        std::uint32_t word;
    };
    const std::array<Case, 6> cases{{
        {"VOP1 SDWA", 0x7e0202f9u},
        {"VOP1 DPP", 0x7e0202fau},
        {"VOP2 SDWA", 0x4a0202f9u},
        {"VOP2 DPP", 0x4a0202fau},
        {"VOPC SDWA", 0x7c0202f9u},
        {"VOPC DPP", 0x7c0202fau},
    }};
    int failures = 0;
    for (const auto& entry : cases) {
        const std::array<std::uint32_t, 2> words{entry.word, 0xaf00e400u};
        const std::string expected = std::string("truncated ") + entry.name + " instruction";
        try {
            static_cast<void>(RdnaInstructionDecoder{}.Decode(std::span(words).first(1)));
            std::fprintf(stderr, "%s: a missing modifier word decodes\n", entry.name);
            ++failures;
        } catch (const std::out_of_range& error) {
            if (expected != error.what()) {
                std::fprintf(stderr, "%s: %s\n", entry.name, error.what());
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
