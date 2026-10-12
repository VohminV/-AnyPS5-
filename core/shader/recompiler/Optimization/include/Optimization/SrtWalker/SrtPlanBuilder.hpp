#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTPLANBUILDER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTPLANBUILDER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace ShaderRecompiler::Detail {

class PlanBuilder {
public:
    explicit PlanBuilder(IrProgram& program) : _program(program) {}

    void Run();

private:
    struct Patch {
        IrValue* inst = nullptr;
        std::uint32_t slot = 0;
        bool keep = false;
    };

    void Collect(IrValue* raw, std::uint32_t usePc);
    void PatchReads();

    IrProgram& _program;
    // Current DFS path, in order (needed for cycle slices); mirrored by
    // _visitingSet for O(1) membership: linear finds here scaled with the
    // path depth and dominated monster-shader preparation time.
    std::vector<IrValue*> _visiting;
    std::unordered_set<IrValue*> _visitingSet;
    // Completed values; order is never observed, only membership.
    std::unordered_set<IrValue*> _visited;
    std::vector<Patch> _patches;
};

}

#endif
