#include "prx/libSceAgcDriver/Graphics/include/DepthBoundsFallback.hpp"

#include <spirv/unified1/spirv.hpp>

#include <atomic>
#include <chrono>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace AgcDriver::Graphics {
namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203;
// The backend declares the push block as u32[32] (see ShaderValidation);
// the fallback reads the last two dwords of that block.
constexpr std::uint32_t kPushBlockDwords = 32;

[[noreturn]] void Unsupported(const std::string& reason) {
    throw std::runtime_error("depth bounds fallback unsupported: " + reason);
}

bool EqualsIgnoreCase(std::string_view left, const char* right) {
    std::size_t index = 0;
    for (; index < left.size() && right[index] != '\0'; ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) != right[index]) return false;
    }
    return index == left.size() && right[index] == '\0';
}

constexpr std::uint16_t OpCode(std::uint32_t word) { return static_cast<std::uint16_t>(word & 0xffffu); }
constexpr std::uint16_t OpWords(std::uint32_t word) { return static_cast<std::uint16_t>(word >> 16u); }

struct Insn {
    std::size_t pos = 0;
    std::uint16_t op = 0;
    std::uint16_t count = 0;
};

} // namespace

DepthBoundsBackend ResolveDepthBoundsBackend(bool supported, std::string_view override) {
    if (override.empty() || EqualsIgnoreCase(override, "auto")) return supported ? DepthBoundsBackend::Hardware : DepthBoundsBackend::CpuFallback;
    if (EqualsIgnoreCase(override, "hardware")) {
        if (!supported) throw std::runtime_error("APS5_DEPTH_BOUNDS=hardware but the device does not support depthBounds");
        return DepthBoundsBackend::Hardware;
    }
    if (EqualsIgnoreCase(override, "cpu")) return DepthBoundsBackend::CpuFallback;
    throw std::runtime_error("APS5_DEPTH_BOUNDS must be auto, hardware or cpu");
}

const char* DepthBoundsBackendText(DepthBoundsBackend backend) {
    return backend == DepthBoundsBackend::Hardware ? "hardware" : "cpu-fallback";
}

namespace {

std::atomic<std::uint64_t>& FallbackPipelines() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}
std::atomic<std::uint64_t>& FallbackTransforms() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}
std::atomic<std::uint64_t>& FallbackRefusals() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}
std::atomic<std::uint64_t>& FallbackTransformMicroseconds() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

} // namespace

void NoteDepthBoundsFallbackPipeline(std::uint64_t transformMicroseconds) {
    ++FallbackPipelines();
    FallbackTransformMicroseconds() += transformMicroseconds;
}

void NoteDepthBoundsFallbackRefusal() {
    ++FallbackRefusals();
}

DepthBoundsFallbackStats GetDepthBoundsFallbackStats() {
    DepthBoundsFallbackStats stats{};
    stats.pipelines = FallbackPipelines().load();
    stats.transforms = FallbackTransforms().load();
    stats.refusals = FallbackRefusals().load();
    stats.transformMicroseconds = FallbackTransformMicroseconds().load();
    return stats;
}

namespace {

// A minimal SPIR-V reader: enough to find the fragment entry point, its
// interface, the push block and FragCoord, and to splice a bounds-check
// prologue. Anything unexpected refuses loudly instead of guessing.
struct Reader {
    std::span<const std::uint32_t> words;
    std::vector<Insn> insns;

    explicit Reader(std::span<const std::uint32_t> input) : words(input) {
        if (words.size() < 5 || words[0] != kSpirvMagic) Unsupported("not SPIR-V");
        std::size_t pos = 5;
        while (pos < words.size()) {
            const std::uint16_t op = OpCode(words[pos]);
            const std::uint16_t count = OpWords(words[pos]);
            if (count == 0 || pos + count > words.size()) Unsupported("truncated instruction");
            insns.push_back({pos, op, count});
            pos += count;
        }
        if (words[3] == 0) Unsupported("empty id bound");
    }

    std::uint32_t Bound() const { return words[3]; }

    // id -> defining instruction for type/constant ids.
    std::unordered_map<std::uint32_t, Insn> definitions;
    void IndexDefinitions() {
        for (const auto& insn : insns) {
            const auto op = static_cast<spv::Op>(insn.op);
            if (op != spv::OpTypeVoid && op != spv::OpTypeBool && op != spv::OpTypeInt && op != spv::OpTypeFloat && op != spv::OpTypeVector &&
                op != spv::OpTypeArray && op != spv::OpTypeStruct && op != spv::OpTypePointer && op != spv::OpConstant) continue;
            if (insn.count < 2) Unsupported("malformed declaration");
            definitions.emplace(words[insn.pos + 1], insn);
        }
    }

    const Insn& Definition(std::uint32_t id) const {
        const auto found = definitions.find(id);
        if (found == definitions.end()) Unsupported("unknown id");
        return found->second;
    }

    // Signature helpers over resolved type instructions.
    bool IsF32(std::uint32_t id) const {
        const auto& type = Definition(id);
        return static_cast<spv::Op>(type.op) == spv::OpTypeFloat && type.count == 3 && words[type.pos + 2] == 32;
    }
    bool IsU32(std::uint32_t id) const {
        const auto& type = Definition(id);
        return static_cast<spv::Op>(type.op) == spv::OpTypeInt && type.count == 4 && words[type.pos + 2] == 32 && words[type.pos + 3] == 0;
    }
    bool IsBool(std::uint32_t id) const {
        const auto& type = Definition(id);
        return static_cast<spv::Op>(type.op) == spv::OpTypeBool && type.count == 2;
    }
    bool IsVec4F32(std::uint32_t id) const {
        const auto& type = Definition(id);
        return static_cast<spv::Op>(type.op) == spv::OpTypeVector && type.count == 4 && words[type.pos + 3] == 4 && IsF32(words[type.pos + 2]);
    }
};

struct Emitter {
    std::vector<std::uint32_t> words;
    std::uint32_t next = 1;

    std::uint32_t Alloc() { return next++; }
    void Instruction(std::uint16_t op, std::initializer_list<std::uint32_t> operands) {
        words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16u) | op);
        for (const auto value : operands) words.push_back(value);
    }
    void Raw(std::uint32_t word) { words.push_back(word); }
};

struct Planned {
    const Insn* entry = nullptr;
    std::uint32_t entryFunc = 0;
    std::vector<std::uint32_t> interface;
    std::uint32_t pushVar = 0;
    std::uint32_t fragVar = 0;
    std::uint32_t f32 = 0, u32 = 0, boolean = 0, v4f32 = 0, ptrInputV4 = 0, ptrPushU32 = 0, ptrPushStruct = 0;
    std::uint32_t pushArr = 0, pushStruct = 0, pushLen = 0;
    std::uint32_t const0 = 0, const30 = 0, const31 = 0;
    bool newF32 = false, newU32 = false, newBool = false, newV4 = false, newPtrInput = false, newPtrPush = false, newPtrPushStruct = false;
    bool newArr = false, newStruct = false, newLen = false, newC0 = false, newC30 = false, newC31 = false;
    bool newFragVar = false, newPushVar = false;
    std::uint32_t loadV = 0, coordZ = 0, ptrMin = 0, minU = 0, minF = 0, ptrMax = 0, maxU = 0, maxF = 0;
    std::uint32_t less = 0, greater = 0, kill = 0;
    std::uint32_t entryBlock = 0, killBlock = 0, mergeBlock = 0;
    std::uint32_t oldFirst = 0;
    std::uint32_t bound = 0;
};

Planned Plan(const Reader& reader, std::uint32_t bound) {
    Planned planned;
    planned.bound = bound;
    // --- locate the single fragment entry point.
    for (const auto& insn : reader.insns) {
        if (static_cast<spv::Op>(insn.op) != spv::OpEntryPoint) continue;
        if (insn.count < 4 || reader.words[insn.pos + 1] != spv::ExecutionModelFragment) continue;
        if (planned.entry != nullptr) Unsupported("several fragment entry points");
        planned.entry = &insn;
    }
    if (planned.entry == nullptr) Unsupported("no fragment entry point");
    planned.entryFunc = reader.words[planned.entry->pos + 2];
    std::size_t name = planned.entry->pos + 3;
    for (;;) {
        if (name >= planned.entry->pos + planned.entry->count) Unsupported("malformed entry point");
        const std::uint32_t word = reader.words[name++];
        if ((word & 0xffu) == 0 || ((word >> 8u) & 0xffu) == 0 || ((word >> 16u) & 0xffu) == 0 || ((word >> 24u) & 0xffu) == 0) break;
    }
    planned.interface.assign(reader.words.begin() + name, reader.words.begin() + planned.entry->pos + planned.entry->count);

    // --- depth writes use the written value, not FragCoord: refuse loudly.
    for (const auto& insn : reader.insns) {
        if (static_cast<spv::Op>(insn.op) != spv::OpExecutionMode || insn.count < 3 || reader.words[insn.pos + 1] != planned.entryFunc) continue;
        const auto mode = reader.words[insn.pos + 2];
        if (mode == spv::ExecutionModeDepthReplacing || mode == spv::ExecutionModeDepthLess || mode == spv::ExecutionModeDepthGreater) {
            Unsupported("shader writes depth");
        }
    }

    // --- find the push block (Block struct with one u32[32] member) and FragCoord.
    std::uint32_t pushVar = 0;
    for (const auto& insn : reader.insns) {
        if (static_cast<spv::Op>(insn.op) != spv::OpVariable || insn.count != 4 || reader.words[insn.pos + 3] != spv::StorageClassPushConstant) continue;
        if (pushVar != 0) Unsupported("several push constant blocks");
        pushVar = reader.words[insn.pos + 2];
    }
    if (pushVar != 0) {
        bool ok = false;
        for (const auto& insn : reader.insns) {
            if (static_cast<spv::Op>(insn.op) != spv::OpVariable || insn.count != 4 || reader.words[insn.pos + 2] != pushVar) continue;
            const auto& pointer = reader.Definition(reader.words[insn.pos + 1]);
            if (static_cast<spv::Op>(pointer.op) != spv::OpTypePointer || pointer.count != 4 || reader.words[pointer.pos + 2] != spv::StorageClassPushConstant) break;
            const auto& structure = reader.Definition(reader.words[pointer.pos + 3]);
            if (static_cast<spv::Op>(structure.op) != spv::OpTypeStruct || structure.count != 3) break;
            const auto& array = reader.Definition(reader.words[structure.pos + 2]);
            if (static_cast<spv::Op>(array.op) != spv::OpTypeArray || array.count != 4 || !reader.IsU32(reader.words[array.pos + 2])) break;
            bool lengthOk = false;
            for (const auto& constant : reader.insns) {
                if (static_cast<spv::Op>(constant.op) == spv::OpConstant && constant.count == 4 && reader.words[constant.pos + 1] == reader.words[array.pos + 2] &&
                    reader.words[constant.pos + 2] == reader.words[array.pos + 3] && reader.words[constant.pos + 3] == kPushBlockDwords) {
                    lengthOk = true;
                    break;
                }
            }
            if (!lengthOk) break;
            bool strideOk = false;
            for (const auto& decorate : reader.insns) {
                if (static_cast<spv::Op>(decorate.op) == spv::OpDecorate && decorate.count == 4 && reader.words[decorate.pos + 1] == reader.words[array.pos + 1] &&
                    reader.words[decorate.pos + 2] == spv::DecorationArrayStride && reader.words[decorate.pos + 3] == 4) {
                    strideOk = true;
                    break;
                }
            }
            if (!strideOk) break;
            ok = true;
            break;
        }
        if (!ok) Unsupported("push constant layout is not a u32[32] block");
    }
    std::uint32_t fragVar = 0;
    for (const auto& insn : reader.insns) {
        if (static_cast<spv::Op>(insn.op) != spv::OpVariable || insn.count != 4 || reader.words[insn.pos + 3] != spv::StorageClassInput) continue;
        const std::uint32_t id = reader.words[insn.pos + 2];
        bool builtin = false;
        for (const auto& decorate : reader.insns) {
            if (static_cast<spv::Op>(decorate.op) == spv::OpDecorate && decorate.count == 4 && reader.words[decorate.pos + 1] == id &&
                reader.words[decorate.pos + 2] == spv::DecorationBuiltIn && reader.words[decorate.pos + 3] == spv::BuiltInFragCoord) {
                builtin = true;
                break;
            }
        }
        if (!builtin) continue;
        if (fragVar != 0) Unsupported("several FragCoord inputs");
        const auto& pointer = reader.Definition(reader.words[insn.pos + 1]);
        if (static_cast<spv::Op>(pointer.op) != spv::OpTypePointer || pointer.count != 4 || reader.words[pointer.pos + 2] != spv::StorageClassInput ||
            !reader.IsVec4F32(reader.words[pointer.pos + 3])) {
            Unsupported("FragCoord input is not a vec4");
        }
        fragVar = id;
    }

    // --- carry the scan results into the plan.
    planned.pushVar = pushVar;
    planned.fragVar = fragVar;

    // --- find or plan scalar types and u32 constants 0/30/31.
    const auto findType = [&](auto predicate) -> std::uint32_t {
        for (const auto& [id, insn] : reader.definitions) {
            if (predicate(id, insn)) return id;
        }
        return 0;
    };
    planned.f32 = findType([&](std::uint32_t, const Insn& insn) { return static_cast<spv::Op>(insn.op) == spv::OpTypeFloat && insn.count == 3 && reader.words[insn.pos + 2] == 32; });
    planned.u32 = findType([&](std::uint32_t, const Insn& insn) {
        return static_cast<spv::Op>(insn.op) == spv::OpTypeInt && insn.count == 4 && reader.words[insn.pos + 2] == 32 && reader.words[insn.pos + 3] == 0;
    });
    planned.boolean = findType([&](std::uint32_t, const Insn& insn) { return static_cast<spv::Op>(insn.op) == spv::OpTypeBool && insn.count == 2; });
    if (planned.f32 != 0) {
        planned.v4f32 = findType([&](std::uint32_t, const Insn& insn) {
            return static_cast<spv::Op>(insn.op) == spv::OpTypeVector && insn.count == 4 && reader.words[insn.pos + 2] == planned.f32 && reader.words[insn.pos + 3] == 4;
        });
    }
    if (planned.v4f32 != 0) {
        planned.ptrInputV4 = findType([&](std::uint32_t, const Insn& insn) {
            return static_cast<spv::Op>(insn.op) == spv::OpTypePointer && insn.count == 4 && reader.words[insn.pos + 2] == spv::StorageClassInput &&
                reader.words[insn.pos + 3] == planned.v4f32;
        });
    }
    if (planned.u32 != 0) {
        planned.ptrPushU32 = findType([&](std::uint32_t, const Insn& insn) {
            return static_cast<spv::Op>(insn.op) == spv::OpTypePointer && insn.count == 4 && reader.words[insn.pos + 2] == spv::StorageClassPushConstant &&
                reader.words[insn.pos + 3] == planned.u32;
        });
        for (const auto& insn : reader.insns) {
            if (static_cast<spv::Op>(insn.op) != spv::OpConstant || insn.count != 4 || reader.words[insn.pos + 1] != planned.u32) continue;
            if (reader.words[insn.pos + 3] == 0) planned.const0 = reader.words[insn.pos + 2];
            if (reader.words[insn.pos + 3] == DepthBoundsPushMinDword) planned.const30 = reader.words[insn.pos + 2];
            if (reader.words[insn.pos + 3] == DepthBoundsPushMaxDword) planned.const31 = reader.words[insn.pos + 2];
        }
    }

    // --- allocate fresh ids for everything missing.
    std::uint32_t next = bound;
    auto take = [&](std::uint32_t& slot, bool& fresh) -> std::uint32_t {
        if (slot != 0) {
            fresh = false;
            return slot;
        }
        if (next >= 0xffffff00u) Unsupported("id space exhausted");
        fresh = true;
        return slot = next++;
    };
    take(planned.f32, planned.newF32);
    take(planned.u32, planned.newU32);
    take(planned.boolean, planned.newBool);
    take(planned.v4f32, planned.newV4);
    take(planned.ptrInputV4, planned.newPtrInput);
    take(planned.ptrPushU32, planned.newPtrPush);
    take(planned.const0, planned.newC0);
    take(planned.const30, planned.newC30);
    take(planned.const31, planned.newC31);
    bool pushVarFound = planned.pushVar != 0;
    if (!pushVarFound) {
        take(planned.pushArr, planned.newArr);
        take(planned.pushStruct, planned.newStruct);
        take(planned.pushLen, planned.newLen);
        take(planned.ptrPushStruct, planned.newPtrPushStruct);
        take(planned.pushVar, planned.newPushVar);
    }
    bool fragVarFound = planned.fragVar != 0;
    if (!fragVarFound) take(planned.fragVar, planned.newFragVar);
    planned.loadV = next++;
    planned.coordZ = next++;
    planned.ptrMin = next++;
    planned.minU = next++;
    planned.minF = next++;
    planned.ptrMax = next++;
    planned.maxU = next++;
    planned.maxF = next++;
    planned.less = next++;
    planned.greater = next++;
    planned.kill = next++;
    planned.entryBlock = next++;
    planned.killBlock = next++;
    planned.mergeBlock = next++;
    planned.bound = next;

    // --- the entry function's first block must be a plain label: splicing a
    // new predecessor would otherwise invalidate its phi instructions.
    bool inEntryFunc = false;
    bool firstBlockSeen = false;
    for (const auto& insn : reader.insns) {
        const auto opcode = static_cast<spv::Op>(insn.op);
        if (opcode == spv::OpFunction) {
            inEntryFunc = insn.count >= 3 && reader.words[insn.pos + 2] == planned.entryFunc;
            firstBlockSeen = false;
            continue;
        }
        if (!inEntryFunc) continue;
        if (opcode == spv::OpFunctionParameter) continue;
        if (!firstBlockSeen) {
            // Line debug info may precede the first label.
            if (opcode == spv::OpLine || opcode == spv::OpNoLine) continue;
            if (opcode != spv::OpLabel) Unsupported("entry function has no leading label");
            planned.oldFirst = reader.words[insn.pos + 1];
            firstBlockSeen = true;
            continue;
        }
        if (opcode == spv::OpPhi) Unsupported("entry block with phi instructions");
        if (opcode == spv::OpBranch || opcode == spv::OpBranchConditional || opcode == spv::OpSwitch || opcode == spv::OpKill ||
            opcode == spv::OpReturn || opcode == spv::OpReturnValue || opcode == spv::OpUnreachable) {
            break;
        }
    }
    if (planned.oldFirst == 0) Unsupported("entry function has no blocks");
    return planned;
}

bool IsSection9(std::uint16_t opcode) {
    const auto op = static_cast<spv::Op>(opcode);
    switch (op) {
        case spv::OpTypeVoid:
        case spv::OpTypeBool:
        case spv::OpTypeInt:
        case spv::OpTypeFloat:
        case spv::OpTypeVector:
        case spv::OpTypeMatrix:
        case spv::OpTypeImage:
        case spv::OpTypeSampler:
        case spv::OpTypeSampledImage:
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
        case spv::OpTypeStruct:
        case spv::OpTypeOpaque:
        case spv::OpTypePointer:
        case spv::OpTypeFunction:
        case spv::OpTypeEvent:
        case spv::OpTypeDeviceEvent:
        case spv::OpTypeReserveId:
        case spv::OpTypeQueue:
        case spv::OpTypePipe:
        case spv::OpTypeForwardPointer:
        case spv::OpConstantTrue:
        case spv::OpConstantFalse:
        case spv::OpConstant:
        case spv::OpConstantComposite:
        case spv::OpConstantSampler:
        case spv::OpConstantNull:
        case spv::OpSpecConstantTrue:
        case spv::OpSpecConstantFalse:
        case spv::OpSpecConstant:
        case spv::OpSpecConstantComposite:
        case spv::OpSpecConstantOp:
        case spv::OpVariable:
        case spv::OpUndef:
            return true;
        default:
            return false;
    }
}

std::vector<std::uint32_t> Emit(const Reader& reader, const Planned& planned) {
    std::vector<std::uint32_t> out;
    out.reserve(reader.words.size() + 160);
    for (int i = 0; i < 5; ++i) out.push_back(reader.words[i]);
    out[3] = planned.bound;
    const auto op = [&](std::uint16_t opcode, std::initializer_list<std::uint32_t> operands) {
        out.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16u) | opcode);
        for (const auto value : operands) out.push_back(value);
    };
    const auto emitNewDecorates = [&] {
        if (planned.newFragVar) op(spv::OpDecorate, {planned.fragVar, spv::DecorationBuiltIn, spv::BuiltInFragCoord});
        if (planned.newPushVar) {
            op(spv::OpDecorate, {planned.pushArr, spv::DecorationArrayStride, 4});
            op(spv::OpDecorate, {planned.pushStruct, spv::DecorationBlock});
            op(spv::OpMemberDecorate, {planned.pushStruct, 0, spv::DecorationOffset, 0});
        }
    };
    const auto emitNewDecls = [&] {
        if (planned.newF32) op(spv::OpTypeFloat, {planned.f32, 32});
        if (planned.newU32) op(spv::OpTypeInt, {planned.u32, 32, 0});
        if (planned.newBool) op(spv::OpTypeBool, {planned.boolean});
        if (planned.newV4) op(spv::OpTypeVector, {planned.v4f32, planned.f32, 4});
        if (planned.newPtrInput) op(spv::OpTypePointer, {planned.ptrInputV4, spv::StorageClassInput, planned.v4f32});
        if (planned.newPtrPush) op(spv::OpTypePointer, {planned.ptrPushU32, spv::StorageClassPushConstant, planned.u32});
        if (planned.newPtrPushStruct) op(spv::OpTypePointer, {planned.ptrPushStruct, spv::StorageClassPushConstant, planned.pushStruct});
        if (planned.newArr) op(spv::OpTypeArray, {planned.pushArr, planned.u32, planned.pushLen});
        if (planned.newStruct) op(spv::OpTypeStruct, {planned.pushStruct, planned.pushArr});
        if (planned.newLen) op(spv::OpConstant, {planned.u32, planned.pushLen, kPushBlockDwords});
        if (planned.newC0) op(spv::OpConstant, {planned.u32, planned.const0, 0});
        if (planned.newC30) op(spv::OpConstant, {planned.u32, planned.const30, DepthBoundsPushMinDword});
        if (planned.newC31) op(spv::OpConstant, {planned.u32, planned.const31, DepthBoundsPushMaxDword});
        if (planned.newFragVar) op(spv::OpVariable, {planned.ptrInputV4, planned.fragVar, spv::StorageClassInput});
        if (planned.newPushVar) op(spv::OpVariable, {planned.ptrPushStruct, planned.pushVar, spv::StorageClassPushConstant});
    };
    const auto emitPrologue = [&] {
        op(spv::OpLabel, {planned.entryBlock});
        op(spv::OpLoad, {planned.v4f32, planned.loadV, planned.fragVar});
        op(spv::OpCompositeExtract, {planned.f32, planned.coordZ, planned.loadV, 2});
        op(spv::OpAccessChain, {planned.ptrPushU32, planned.ptrMin, planned.pushVar, planned.const0, planned.const30});
        op(spv::OpLoad, {planned.u32, planned.minU, planned.ptrMin});
        op(spv::OpBitcast, {planned.f32, planned.minF, planned.minU});
        op(spv::OpAccessChain, {planned.ptrPushU32, planned.ptrMax, planned.pushVar, planned.const0, planned.const31});
        op(spv::OpLoad, {planned.u32, planned.maxU, planned.ptrMax});
        op(spv::OpBitcast, {planned.f32, planned.maxF, planned.maxU});
        op(spv::OpFOrdLessThan, {planned.boolean, planned.less, planned.coordZ, planned.minF});
        op(spv::OpFOrdGreaterThan, {planned.boolean, planned.greater, planned.coordZ, planned.maxF});
        op(spv::OpLogicalOr, {planned.boolean, planned.kill, planned.less, planned.greater});
        op(spv::OpSelectionMerge, {planned.mergeBlock, 0});
        op(spv::OpBranchConditional, {planned.kill, planned.killBlock, planned.mergeBlock});
        op(spv::OpLabel, {planned.killBlock});
        op(spv::OpKill, {});
        op(spv::OpLabel, {planned.mergeBlock});
        op(spv::OpBranch, {planned.oldFirst});
    };
    bool decoratesFlushed = false;
    bool declsFlushed = false;
    bool prologueDone = false;
    for (const auto& insn : reader.insns) {
        const auto opcode = static_cast<spv::Op>(insn.op);
        if (!decoratesFlushed && IsSection9(insn.op)) {
            decoratesFlushed = true;
            emitNewDecorates();
        }
        if (static_cast<spv::Op>(insn.op) == spv::OpEntryPoint && insn.pos == planned.entry->pos && planned.newFragVar) {
            out.push_back(((insn.count + 1) << 16u) | insn.op);
            for (std::uint16_t i = 1; i < insn.count; ++i) out.push_back(reader.words[insn.pos + i]);
            out.push_back(planned.fragVar);
            continue;
        }
        if (!declsFlushed && opcode == spv::OpFunction) {
            declsFlushed = true;
            emitNewDecls();
        }
        for (std::uint16_t i = 0; i < insn.count; ++i) out.push_back(reader.words[insn.pos + i]);
        if (!prologueDone && opcode == spv::OpFunction && insn.count >= 3 && reader.words[insn.pos + 2] == planned.entryFunc) {
            prologueDone = true;
            emitPrologue();
        }
    }
    return out;
}

std::vector<std::uint32_t> Transform(const Reader& reader, std::uint32_t bound) {
    Planned planned = Plan(reader, bound);
    return Emit(reader, planned);
}

} // namespace

std::vector<std::uint32_t> ApplyDepthBoundsFallback(std::span<const std::uint32_t> spirv) {
    const auto start = std::chrono::steady_clock::now();
    Reader reader(spirv);
    reader.IndexDefinitions();
    try {
        auto out = Transform(reader, reader.Bound());
        ++FallbackTransforms();
        FallbackTransformMicroseconds() += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
        return out;
    } catch (...) {
        ++FallbackRefusals();
        throw;
    }
}

} // namespace AgcDriver::Graphics
