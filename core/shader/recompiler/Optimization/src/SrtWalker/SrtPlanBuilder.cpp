#include "Optimization/SrtWalker/SrtPlanBuilder.hpp"
#include "Optimization/SrtWalker/SrtDiagnostics.hpp"
#include "Optimization/SrtWalker/SrtInstructionPredicates.hpp"
#include "Optimization/SrtWalker/SrtRuntimeValidator.hpp"

#include "IntermediateRepresentation/IrBuilder.hpp"

#include <algorithm>
#include <stdexcept>

namespace ShaderRecompiler::Detail {

void PlanBuilder::Run() {
    _program.Resources().srtReads.clear();
    _program.Metadata().dynamicReads.clear();
    for (auto& block : _program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            const auto op = inst->Opcode();
            if (op == IrOpcode::LoadAddressU32 || op == IrOpcode::ReadConstBuffer) {
                const auto flags = inst->Flags<MemoryFlags>();
                if (flags.index < _program.Resources().memoryInfo.size()) {
                    const auto kind = _program.Resources().memoryInfo[flags.index].kind;
                    const bool crosswired = (op == IrOpcode::LoadAddressU32 && kind == ResourceKind::ScalarBuffer) || (op == IrOpcode::ReadConstBuffer && kind == ResourceKind::ScalarAddress);
                    if (crosswired) {
                        Fail(_program.Resources(), flags.pc, std::string(IrOpcodeName(op)) + " has incompatible scalar memory metadata");
                    }
                }
            }
            if (IsDescriptorHandle(inst->Opcode())) {
                for (std::size_t index = 0; index < inst->ArgumentCount(); index++) {
                    Collect(inst->Argument(index), 0);
                }
            }
        }
    }
    for (auto& block : _program.Blocks()) {
        for (IrValue* inst : block->Instructions()) {
            if (inst->Opcode() == IrOpcode::LoadAddressU32 && IsRawRead(_program.Resources(), *inst) && inst->Argument(1)->Resolve()->HasImmediate() && RuntimeValidator(_program.Resources(), RuntimeValueType::Any).Run(inst)) {
                Collect(inst, inst->Flags<MemoryFlags>().pc);
            }
        }
    }
    PatchReads();
}

void PlanBuilder::Collect(IrValue* raw, std::uint32_t usePc) {
    // Iterative post-order DFS: the recursion it replaces overflowed the
    // stack on shaders with use-def chains thousands deep (a 1 MB worker
    // stack fits only a few thousand frames). Traversal order, cycle
    // handling and memoization match the recursive formulation exactly, so
    // slot assignment is unchanged.
    struct Frame {
        IrValue* inst;
        std::size_t nextChild;
    };
    std::vector<Frame> stack;
    const auto push = [&](IrValue* rawValue) -> bool {
        IrValue* value = rawValue->Resolve();
        if (value->Opcode() == IrOpcode::Void) {
            return false;
        }
        IrValue* inst = value;
        if (_visitingSet.find(inst) != _visitingSet.end()) {
            const auto cycle = std::find(_visiting.begin(), _visiting.end(), inst);
            const auto containsPhi = std::any_of(cycle, _visiting.end(), [](IrValue* candidate) { return candidate->Opcode() == IrOpcode::Phi; });
            if (containsPhi) {
                return false;
            }
            Fail(_program.Resources(), usePc, "cyclic typed planning value " + std::string(IrOpcodeName(inst->Opcode())) + " without a phi");
        }
        if (_visited.find(inst) != _visited.end()) {
            return false;
        }
        _visiting.push_back(inst);
        _visitingSet.insert(inst);
        stack.push_back({inst, 0});
        return true;
    };
    const auto finish = [&](Frame& frame) {
        _visiting.pop_back();
        _visitingSet.erase(frame.inst);
        _visited.insert(frame.inst);
        if (!IsRawRead(_program.Resources(), *frame.inst)) {
            return;
        }
        IrValue* offset = frame.inst->Argument(1)->Resolve();
        const auto foldable = offset->HasImmediate() && offset->Type() == IrType::U32 && RuntimeValidator(_program.Resources(), RuntimeValueType::Any).Run(frame.inst);
        if (!foldable) {
            if (std::find(_program.Metadata().dynamicReads.begin(), _program.Metadata().dynamicReads.end(), frame.inst) == _program.Metadata().dynamicReads.end()) {
                _program.Metadata().dynamicReads.push_back(frame.inst);
            }
            return;
        }
        for (std::uint32_t slot = 0; slot < _program.Resources().srtReads.size(); slot++) {
            if (EquivalentValue(_program.Resources(), frame.inst, _program.Resources().srtReads[slot].value)) {
                _patches.push_back({frame.inst, slot, false});
                return;
            }
        }
        const auto slot = static_cast<std::uint32_t>(_program.Resources().srtReads.size());
        _program.Resources().srtReads.push_back({frame.inst, slot});
        _patches.push_back({frame.inst, slot, true});
    };
    if (!push(raw)) {
        return;
    }
    while (!stack.empty()) {
        const std::size_t top = stack.size() - 1u;
        Frame& frame = stack[top];
        if (frame.nextChild < frame.inst->ArgumentCount()) {
            IrValue* child = frame.inst->Argument(frame.nextChild);
            stack[top].nextChild++;
            push(child);
            continue;
        }
        finish(frame);
        stack.pop_back();
    }
}

void PlanBuilder::PatchReads() {
    IrBuilder builder(_program);
    for (const auto& patch : _patches) {
        IrBlock* block = patch.inst->Parent();
        if (block == nullptr) {
            throw std::runtime_error("SrtWalker::BuildPlan patched value has no parent block");
        }
        builder.SetInsertionPoint(*block);
        IrValue& resource = builder.Emit(IrOpcode::GetSrtResource, IrType::SrtResource, {});
        block->RemoveInstruction(&resource);
        block->InsertInstructionBefore(patch.inst, &resource);
        IrValue& slotConstant = builder.Constant(patch.slot);
        IrValue& flat = builder.Emit(IrOpcode::ReadConst, IrType::U32, {&resource, &slotConstant});
        block->RemoveInstruction(&flat);
        block->InsertInstructionBefore(patch.inst, &flat);
        for (const auto& use : std::vector<IrUse>(patch.inst->OperandUses())) {
            use.user->ReplaceArgument(use.operand, &flat);
        }
        for (auto& info : _program.Metadata().blockInfo) {
            if (info.condition != nullptr && info.condition->Resolve() == patch.inst) {
                info.condition = &flat;
            }
            if (info.indirectTarget != nullptr && info.indirectTarget->Resolve() == patch.inst) {
                info.indirectTarget = &flat;
            }
        }
        if (patch.keep) {
            const auto memory = patch.inst->Flags<MemoryFlags>().index;
            if (memory < _program.Resources().memoryInfo.size()) {
                _program.Resources().memoryInfo[memory].planningOnly = true;
            }
            const auto& irVal = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {patch.inst});
        }
    }
}

}
