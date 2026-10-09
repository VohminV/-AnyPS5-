 #include "SelfInspector.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace SelfInspector {

namespace {

constexpr std::uint32_t kElfMagic = 0x464C457F;
constexpr std::uint32_t kSelfAppMagic = 0x1D3D154F;
constexpr std::uint32_t kSelfPkgMagic = 0xEEF51454;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynamic = 2;
constexpr std::uint64_t kMaxInputSize = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kRecordsBase = 0x40;
constexpr std::uint64_t kRecordSize = 32;
constexpr std::uint64_t kMaxRecords = 256;
constexpr std::uint64_t kMaxPhnum = 64;
constexpr std::uint64_t kMaxElfHits = 64;

bool AddFits(std::uint64_t a, std::uint64_t b, std::uint64_t limit, std::uint64_t& out) {
    if (b > limit || a > limit - b) return false;
    out = a + b;
    return true;
}

std::uint16_t ReadU16Le(const std::vector<std::uint8_t>& data, std::uint64_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, 2);
    return value;
}

std::uint32_t ReadU32Le(const std::vector<std::uint8_t>& data, std::uint64_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}

std::uint64_t ReadU64Le(const std::vector<std::uint8_t>& data, std::uint64_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, data.data() + offset, 8);
    return value;
}

std::string HexU32(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
    return buffer;
}

std::string HexU64(std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20 && c <= 0x7E) {
                out += c;
            } else {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c & 0xFF);
                out += buffer;
            }
        }
    }
    return out;
}

EmbeddedElf ParseEmbeddedElf(const std::vector<std::uint8_t>& data, std::uint64_t base) {
    EmbeddedElf elf;
    elf.fileOffset = base;
    std::uint64_t headerEnd = 0;
    if (!AddFits(base, 64, data.size(), headerEnd)) {
        elf.issues.push_back("truncated ELF header");
        return elf;
    }
    const std::uint8_t* header = data.data() + base;
    if (header[4] != 2) elf.issues.push_back("not 64-bit class");
    if (header[5] != 1) elf.issues.push_back("not little-endian");
    if (header[6] != 1) elf.issues.push_back("bad ELF version");
    elf.type = ReadU16Le(data, base + 16);
    elf.machine = ReadU16Le(data, base + 18);
    if (ReadU32Le(data, base + 20) != 1) elf.issues.push_back("bad header version field");
    elf.entry = ReadU64Le(data, base + 24);
    elf.progHeaderOffset = ReadU64Le(data, base + 32);
    elf.sectHeaderOffset = ReadU64Le(data, base + 40);
    if (ReadU16Le(data, base + 52) != 64) elf.issues.push_back("bad ehsize");
    const std::uint16_t phentsize = ReadU16Le(data, base + 54);
    elf.progHeaderCount = ReadU16Le(data, base + 56);
    if (phentsize != 56 && elf.progHeaderCount != 0) elf.issues.push_back("bad program header entry size");
    if (elf.progHeaderCount > kMaxPhnum) elf.issues.push_back("program header count exceeds sanity limit");

    std::uint64_t tableEnd = 0;
    if (elf.progHeaderCount != 0 &&
        (static_cast<std::uint64_t>(elf.progHeaderCount) > (0xFFFFFFFFFFFFFFFFull / 56) ||
         !AddFits(elf.progHeaderOffset, static_cast<std::uint64_t>(elf.progHeaderCount) * 56, data.size(), tableEnd) ||
         base > data.size() - tableEnd)) {
        elf.issues.push_back("program header table out of input bounds");
        return elf;
    }
    std::uint64_t span = 0;
    bool rangesOk = true;
    for (std::uint16_t i = 0; i < elf.progHeaderCount; ++i) {
        const std::uint64_t at = base + elf.progHeaderOffset + static_cast<std::uint64_t>(i) * 56;
        ProgHeader ph;
        ph.type = ReadU32Le(data, at);
        ph.flags = ReadU32Le(data, at + 4);
        ph.offset = ReadU64Le(data, at + 8);
        ph.vaddr = ReadU64Le(data, at + 16);
        ph.filesz = ReadU64Le(data, at + 32);
        ph.memsz = ReadU64Le(data, at + 40);
        elf.phdrs.push_back(ph);
        std::uint64_t end = 0;
        // p_offset/p_filesz describe the OUTPUT layout. For a plain ELF input
        // (base == 0) they must already fit; for an embedded ELF they are
        // verified after the record remap instead.
        if (!AddFits(ph.offset, ph.filesz, UINT64_MAX, end)) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "program header %u range overflows (type 0x%x)", i, ph.type);
            elf.issues.push_back(buffer);
            rangesOk = false;
            continue;
        }
        if (base == 0) {
            std::uint64_t fileEnd = 0;
            if (!AddFits(end, base, data.size(), fileEnd)) {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "program header %u file range out of input bounds (type 0x%x)", i, ph.type);
                elf.issues.push_back(buffer);
                rangesOk = false;
                continue;
            }
        }
        if (end > span) span = end;
    }
    elf.span = span;
    elf.rangesVerified = rangesOk;
    if (span == 0 && elf.progHeaderCount != 0) {
        elf.issues.push_back("empty program header span");
    }
    if (span != 0 && base == 0) {
        std::uint64_t sliceEnd = 0;
        if (!AddFits(base, span, data.size(), sliceEnd)) {
            elf.issues.push_back("ELF span exceeds input bounds");
        }
    }
    elf.headersValid = elf.issues.empty();
    return elf;
}

bool RangesOverlap(std::uint64_t aStart, std::uint64_t aSize, std::uint64_t bStart, std::uint64_t bSize) {
    if (aSize == 0 || bSize == 0) return false;
    return aStart < bStart + bSize && bStart < aStart + aSize;
}

} // namespace

const char* ContainerKindName(ContainerKind kind) {
    switch (kind) {
    case ContainerKind::TooSmall: return "TOO_SMALL";
    case ContainerKind::PlainElf: return "ELF";
    case ContainerKind::SelfApp: return "SELF";
    case ContainerKind::SelfPkg: return "SELF";
    case ContainerKind::Pkg: return "PKG";
    case ContainerKind::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open input file";
        return false;
    }
    const std::streampos end = file.tellg();
    if (end < 0) {
        error = "cannot stat input file";
        return false;
    }
    const auto size = static_cast<std::uint64_t>(end);
    if (size > kMaxInputSize) {
        error = "input exceeds 512 MiB limit";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size != 0 && !file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        error = "short read of input file";
        return false;
    }
    return true;
}

bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& data, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot open output file";
        return false;
    }
    if (!data.empty() &&
        !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        error = "short write of output file";
        return false;
    }
    file.close();
    if (!file) {
        error = "failed to finalize output file";
        return false;
    }
    return true;
}

double ShannonEntropy(const std::vector<std::uint8_t>& data, std::uint64_t offset, std::uint64_t size) {
    if (size == 0 || offset >= data.size()) return -1.0;
    const std::uint64_t available = data.size() - offset;
    const std::uint64_t count = size < available ? size : available;
    if (count == 0) return -1.0;
    std::uint64_t freq[256] = {};
    for (std::uint64_t i = 0; i < count; ++i) freq[data[offset + i]]++;
    double entropy = 0.0;
    for (int i = 0; i < 256; ++i) {
        if (freq[i] == 0) continue;
        const double p = static_cast<double>(freq[i]) / static_cast<double>(count);
        entropy -= p * (std::log(p) / std::log(2.0));
    }
    return entropy;
}

Analysis AnalyzeBuffer(const std::vector<std::uint8_t>& data) {
    Analysis analysis;
    analysis.fileSize = data.size();
    if (data.size() < 4) {
        analysis.container = ContainerKind::TooSmall;
        analysis.error = "file smaller than 4 bytes: no magic to classify";
        return analysis;
    }
    analysis.magic = ReadU32Le(data, 0);
    switch (analysis.magic) {
    case kElfMagic: analysis.container = ContainerKind::PlainElf; break;
    case kSelfAppMagic: analysis.container = ContainerKind::SelfApp; break;
    case kSelfPkgMagic: analysis.container = ContainerKind::SelfPkg; break;
    default: break;
    }
    if (analysis.container == ContainerKind::Unknown) {
        if (data[0] == 0x7F && data[1] == 'C' && data[2] == 'N' && data[3] == 'T') {
            analysis.container = ContainerKind::Pkg;
            analysis.error = "PKG container: not analyzed, not an executable image";
            return analysis;
        }
        if (data.size() < 0x40) {
            analysis.container = ContainerKind::TooSmall;
            analysis.error = "file too small for a header";
            return analysis;
        }
        analysis.error = "unrecognized magic";
        return analysis;
    }
    if (analysis.container == ContainerKind::PlainElf) {
        analysis.elfs.push_back(ParseEmbeddedElf(data, 0));
        analysis.ok = analysis.elfs.front().headersValid;
        if (!analysis.ok) analysis.error = "plain ELF with invalid headers";
        return analysis;
    }
    if (data.size() < 0x40) {
        analysis.container = ContainerKind::TooSmall;
        analysis.error = "SELF smaller than 0x40 prologue bytes";
        return analysis;
    }
    analysis.hasPrologue = true;
    for (int i = 0; i < 16; ++i) analysis.prologue[i] = ReadU32Le(data, static_cast<std::uint64_t>(i) * 4);

    std::uint64_t elfBase = data.size();
    for (std::uint64_t off = kRecordsBase; off + 4 <= data.size(); ++off) {
        if (ReadU32Le(data, off) == kElfMagic) {
            elfBase = off;
            break;
        }
    }
    if (elfBase == data.size()) {
        analysis.error = "no embedded ELF magic in the file: unsupported or still-encrypted layout";
        return analysis;
    }
    if (elfBase < kRecordsBase + kRecordSize) {
        analysis.error = "embedded ELF overlaps the prologue: no room for segment records";
        return analysis;
    }
    if ((elfBase - kRecordsBase) % kRecordSize != 0) {
        analysis.recordsRemainder = (elfBase - kRecordsBase) % kRecordSize;
    }
    bool anyMismatch = false;
    bool anyEncrypted = false;
    for (std::uint64_t off = kRecordsBase;
         off + kRecordSize <= elfBase && analysis.records.size() < kMaxRecords;
         off += kRecordSize) {
        SelfRecord record;
        record.word0 = ReadU64Le(data, off);
        record.word1 = ReadU64Le(data, off + 8);
        record.word2 = ReadU64Le(data, off + 16);
        record.word3 = ReadU64Le(data, off + 24);
        record.pairEqual = (record.word2 == record.word3);
        record.id = record.word0 >> 20;
        record.dataSegment = (record.word0 & 0x800) != 0;
        record.encrypted = (record.word0 & 0x2) != 0;
        if (!record.pairEqual) anyMismatch = true;
        if (record.encrypted) anyEncrypted = true;
        analysis.records.push_back(record);
    }
    analysis.anyRecordPairMismatch = anyMismatch;
    analysis.anyEncryptedRecord = anyEncrypted;

    for (std::uint64_t off = elfBase; off + 4 <= data.size() && analysis.elfs.size() < kMaxElfHits; ++off) {
        if (ReadU32Le(data, off) == kElfMagic) {
            analysis.elfs.push_back(ParseEmbeddedElf(data, off));
        }
    }
    if (analysis.elfs.size() == kMaxElfHits) analysis.elfHitsCapped = 1;
    if (!analysis.elfs.empty()) {
        const EmbeddedElf& first = analysis.elfs.front();
        if (first.headersValid && first.fileOffset + first.span <= data.size()) {
            analysis.payloadSampleEntropy = ShannonEntropy(data, first.fileOffset, 0x4000);
        }
    }
    analysis.ok = !analysis.elfs.empty() && analysis.elfs.front().headersValid;
    if (!analysis.ok && analysis.error.empty()) {
        analysis.error = "embedded ELF headers invalid: see issues";
    }
    analysis.keysUsed = false;
    return analysis;
}

bool ReassembleElf(const std::vector<std::uint8_t>& data, const Analysis& analysis,
                   std::vector<std::uint8_t>& out, std::string& error) {
    if (analysis.container != ContainerKind::SelfApp && analysis.container != ContainerKind::SelfPkg) {
        error = "reassembly refused: input is not a SELF container";
        return false;
    }
    if (analysis.elfs.empty() || !analysis.elfs.front().headersValid) {
        error = "reassembly refused: no valid embedded ELF";
        return false;
    }
    if (analysis.anyEncryptedRecord) {
        error = "reassembly refused: a segment record carries the encrypted flag; no crypto is implemented";
        return false;
    }
    const EmbeddedElf& elf = analysis.elfs.front();
    if (elf.span == 0) {
        error = "reassembly refused: empty ELF span";
        return false;
    }
    // NOTE: for an embedded ELF, span describes the OUTPUT layout and may
    // exceed the input size; record sources are bounds-checked below instead.
    if (elf.span > kMaxInputSize) {
        error = "reassembly refused: ELF span exceeds 512 MiB limit";
        return false;
    }
    // Header region preserved verbatim: ehdr + program header table.
    std::uint64_t headerEnd = 64;
    std::uint64_t tableEnd = 0;
    if (!AddFits(elf.progHeaderOffset, static_cast<std::uint64_t>(elf.progHeaderCount) * 56, elf.span, tableEnd)) {
        error = "reassembly refused: program header table exceeds ELF span";
        return false;
    }
    if (tableEnd > headerEnd) headerEnd = tableEnd;

    out.assign(static_cast<std::size_t>(elf.span), 0);
    std::uint64_t headerCopyEnd = 0;
    if (!AddFits(elf.fileOffset, headerEnd, data.size(), headerCopyEnd)) {
        error = "reassembly refused: embedded header region out of input bounds";
        return false;
    }
    for (std::uint64_t i = 0; i < headerEnd; ++i) out[i] = data[elf.fileOffset + i];

    Analysis& mutableAnalysis = const_cast<Analysis&>(analysis);
    mutableAnalysis.mapping.clear();
    mutableAnalysis.passthroughBytes = 0;
    mutableAnalysis.headerPreserveEnd = headerEnd;

    auto findDataRecord = [&](std::uint64_t id) -> int {
        int found = -1;
        for (std::size_t i = 0; i < analysis.records.size(); ++i) {
            const SelfRecord& r = analysis.records[i];
            if (!r.dataSegment || r.encrypted) continue;
            if (r.id != id) continue;
            if (!r.pairEqual) continue;
            if (found >= 0) return -2; // ambiguous
            found = static_cast<int>(i);
        }
        return found;
    };

    std::vector<std::pair<std::uint64_t, std::uint64_t>> placed;
    for (std::size_t i = 0; i < elf.phdrs.size(); ++i) {
        const ProgHeader& ph = elf.phdrs[i];
        if (ph.type != kPtLoad || ph.filesz == 0) continue;
        const int recordIndex = findDataRecord(i);
        if (recordIndex < 0) {
            error = "reassembly refused: no unambiguous data record for PT_LOAD program header";
            return false;
        }
        const SelfRecord& record = analysis.records[static_cast<std::size_t>(recordIndex)];
        if (record.word2 != ph.filesz) {
            error = "reassembly refused: record size does not match PT_LOAD file size";
            return false;
        }
        std::uint64_t srcEnd = 0;
        if (!AddFits(record.word1, record.word2, data.size(), srcEnd)) {
            error = "reassembly refused: record source range out of input bounds";
            return false;
        }
        std::uint64_t dstEnd = 0;
        if (!AddFits(ph.offset, ph.filesz, elf.span, dstEnd)) {
            error = "reassembly refused: PT_LOAD target range exceeds ELF span";
            return false;
        }
        if (RangesOverlap(ph.offset, ph.filesz, 0, headerEnd)) {
            error = "reassembly refused: PT_LOAD overlaps the preserved header region";
            return false;
        }
        for (const auto& p : placed) {
            if (RangesOverlap(ph.offset, ph.filesz, p.first, p.second)) {
                error = "reassembly refused: PT_LOAD ranges overlap";
                return false;
            }
        }
        for (std::uint64_t k = 0; k < ph.filesz; ++k) out[ph.offset + k] = data[record.word1 + k];
        placed.emplace_back(ph.offset, ph.filesz);
        SegmentMapping mapping;
        mapping.phdrIndex = static_cast<int>(i);
        mapping.recordIndex = recordIndex;
        mapping.selfOffset = record.word1;
        mapping.elfOffset = ph.offset;
        mapping.size = ph.filesz;
        mutableAnalysis.mapping.push_back(mapping);
    }
    if (placed.empty()) {
        error = "reassembly refused: no PT_LOAD segment mapped";
        return false;
    }
    // Non-LOAD program headers with an id-matching data record (e.g. section
    // 0x6FFFFF00): remap the same way. Anything else keeps zeros: the
    // container bytes at those slice positions are signatures and metadata,
    // not ELF content (for small modules the slice positions do not even
    // exist in the input, so verbatim passthrough would read out of bounds).
    for (std::size_t i = 0; i < elf.phdrs.size(); ++i) {
        const ProgHeader& ph = elf.phdrs[i];
        if (ph.type == kPtLoad || ph.filesz == 0) continue;
        const int recordIndex = findDataRecord(i);
        std::uint64_t dstEnd = 0;
        if (!AddFits(ph.offset, ph.filesz, elf.span, dstEnd)) {
            error = "reassembly refused: program header target range exceeds ELF span";
            return false;
        }
        if (RangesOverlap(ph.offset, ph.filesz, 0, headerEnd)) continue; // inside headers already
        bool covered = false;
        for (const auto& p : placed) {
            if (ph.offset >= p.first && dstEnd <= p.first + p.second) {
                covered = true;
                break;
            }
        }
        if (covered) continue;
        if (recordIndex < 0) continue; // stays zero; documented as unverified gap
        const SelfRecord& record = analysis.records[static_cast<std::size_t>(recordIndex)];
        if (record.word2 != ph.filesz) {
            error = "reassembly refused: record size does not match program header file size";
            return false;
        }
        std::uint64_t srcEnd = 0;
        if (!AddFits(record.word1, record.word2, data.size(), srcEnd)) {
            error = "reassembly refused: record source range out of input bounds";
            return false;
        }
        for (const auto& p : placed) {
            if (RangesOverlap(ph.offset, ph.filesz, p.first, p.second)) {
                error = "reassembly refused: program header partially overlaps a mapped range";
                return false;
            }
        }
        for (std::uint64_t k = 0; k < ph.filesz; ++k) out[ph.offset + k] = data[record.word1 + k];
        placed.emplace_back(ph.offset, ph.filesz);
        SegmentMapping mapping;
        mapping.phdrIndex = static_cast<int>(i);
        mapping.recordIndex = recordIndex;
        mapping.selfOffset = record.word1;
        mapping.elfOffset = ph.offset;
        mapping.size = ph.filesz;
        mutableAnalysis.mapping.push_back(mapping);
    }
    // PT_DYNAMIC sanity: every tag is standard SysV (< 0x40: core tags run to
    // 34, DT_SYMINSZ), OS-range, or the GNU 0x6FFFFFF0 block (e.g. RELACOUNT);
    // the array must be DT_NULL terminated.
    bool dynamicChecked = false;
    for (const auto& ph : elf.phdrs) {
        if (ph.type != kPtDynamic || ph.filesz == 0) continue;
        dynamicChecked = true;
        bool terminated = false;
        for (std::uint64_t off = 0; off + 16 <= ph.filesz; off += 16) {
            const std::int64_t tag = static_cast<std::int64_t>(ReadU64Le(out, ph.offset + off));
            if (tag == 0) {
                terminated = true;
                break;
            }
            const bool standard = tag > 0 && tag < 0x40;
            const bool osRange = tag >= 0x60000000 && tag < 0x62000000;
            const bool gnuRange = tag >= 0x6FFFFFF0 && tag <= 0x6FFFFFFFF;
            if (!standard && !osRange && !gnuRange) {
                error = "reassembly failed verification: PT_DYNAMIC holds an out-of-range tag";
                return false;
            }
        }
        if (!terminated) {
            error = "reassembly failed verification: PT_DYNAMIC is not DT_NULL terminated";
            return false;
        }
    }
    if (!dynamicChecked) {
        error = "reassembly failed verification: no PT_DYNAMIC program header";
        return false;
    }
    const EmbeddedElf check = ParseEmbeddedElf(out, 0);
    if (!check.headersValid || check.progHeaderCount != elf.progHeaderCount || check.span != elf.span) {
        error = "reassembly failed verification: re-parsed output disagrees with source";
        return false;
    }
    return true;
}

std::string BuildJson(const Analysis& analysis, const std::string& inputPath) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"input\": \"" << JsonEscape(inputPath) << "\",\n";
    json << "  \"file_size\": " << analysis.fileSize << ",\n";
    json << "  \"container\": \"" << ContainerKindName(analysis.container) << "\",\n";
    json << "  \"magic\": \"" << HexU32(analysis.magic) << "\",\n";
    json << "  \"ok\": " << (analysis.ok ? "true" : "false") << ",\n";
    json << "  \"error\": \"" << JsonEscape(analysis.error) << "\",\n";
    if (analysis.hasPrologue) {
        json << "  \"prologue_u32le\": [";
        for (int i = 0; i < 16; ++i) {
            if (i != 0) json << ", ";
            json << "\"" << HexU32(analysis.prologue[i]) << "\"";
        }
        json << "],\n";
        json << "  \"prologue_note\": \"raw words only; field semantics UNCONFIRMED (word4 correlates with file size)\",\n";
    }
    json << "  \"records_base\": \"" << HexU64(kRecordsBase) << "\",\n";
    json << "  \"record_size\": " << kRecordSize << ",\n";
    json << "  \"record_count\": " << analysis.records.size() << ",\n";
    json << "  \"records_remainder\": " << analysis.recordsRemainder << ",\n";
    json << "  \"records\": [";
    for (std::size_t i = 0; i < analysis.records.size(); ++i) {
        const SelfRecord& r = analysis.records[i];
        if (i != 0) json << ", ";
        json << "{\"w0\": \"" << HexU64(r.word0) << "\", \"w1\": \"" << HexU64(r.word1)
             << "\", \"w2\": \"" << HexU64(r.word2) << "\", \"w3\": \"" << HexU64(r.word3)
             << "\", \"id\": " << r.id
             << ", \"data_segment\": " << (r.dataSegment ? "true" : "false")
             << ", \"encrypted\": " << (r.encrypted ? "true" : "false")
             << ", \"pair_equal\": " << (r.pairEqual ? "true" : "false") << "}";
    }
    json << "],\n";
    json << "  \"flag_note\": \"id = word0>>20; data = bit 0x800; encrypted = bit 0x2; per PS4 scene docs (PSDevWiki/PSXHAX), UNCONFIRMED for PS5\",\n";
    json << "  \"any_encrypted_record\": " << (analysis.anyEncryptedRecord ? "true" : "false") << ",\n";
    json << "  \"any_record_pair_mismatch\": " << (analysis.anyRecordPairMismatch ? "true" : "false") << ",\n";
    json << "  \"embedded_elfs\": [";
    for (std::size_t i = 0; i < analysis.elfs.size(); ++i) {
        const EmbeddedElf& e = analysis.elfs[i];
        if (i != 0) json << ", ";
        json << "{\"offset\": \"" << HexU64(e.fileOffset) << "\", \"type\": \"" << HexU64(e.type)
             << "\", \"machine\": \"" << HexU64(e.machine) << "\", \"entry\": \"" << HexU64(e.entry)
             << "\", \"phoff\": \"" << HexU64(e.progHeaderOffset)              << "\", \"phnum\": " << e.progHeaderCount
             << ", \"shoff\": \"" << HexU64(e.sectHeaderOffset) << "\", \"span\": \"" << HexU64(e.span)
             << "\", \"headers_valid\": " << (e.headersValid ? "true" : "false") << ", \"phdrs\": [";
        for (std::size_t j = 0; j < e.phdrs.size(); ++j) {
            const ProgHeader& p = e.phdrs[j];
            if (j != 0) json << ", ";
            json << "{\"type\": \"" << HexU64(p.type) << "\", \"flags\": \"" << HexU64(p.flags)
                 << "\", \"offset\": \"" << HexU64(p.offset) << "\", \"vaddr\": \"" << HexU64(p.vaddr)
                 << "\", \"filesz\": \"" << HexU64(p.filesz) << "\", \"memsz\": \"" << HexU64(p.memsz) << "\"}";
        }
        json << "], \"issues\": [";
        for (std::size_t j = 0; j < e.issues.size(); ++j) {
            if (j != 0) json << ", ";
            json << "\"" << JsonEscape(e.issues[j]) << "\"";
        }
        json << "]}";
    }
    json << "],\n";
    json << "  \"elf_hits_capped\": " << analysis.elfHitsCapped << ",\n";
    json << "  \"reassembly_model\": \"plaintext record remap by program header index (id = word0>>20), size match required; no crypto\",\n";
    json << "  \"segment_mapping\": [";
    for (std::size_t i = 0; i < analysis.mapping.size(); ++i) {
        const SegmentMapping& m = analysis.mapping[i];
        if (i != 0) json << ", ";
        json << "{\"phdr\": " << m.phdrIndex << ", \"record\": " << m.recordIndex
             << ", \"self_offset\": \"" << HexU64(m.selfOffset) << "\", \"elf_offset\": \"" << HexU64(m.elfOffset)
             << "\", \"size\": \"" << HexU64(m.size) << "\"}";
    }
    json << "],\n";
    json << "  \"header_preserve_end\": \"" << HexU64(analysis.headerPreserveEnd) << "\",\n";
    json << "  \"passthrough_bytes\": " << analysis.passthroughBytes << ",\n";
    json << "  \"gap_note\": \"headers and mapped records are exact; all other output bytes are zero (container bytes there are signatures/metadata, not ELF content)\",\n";
    json << "  \"decryption\": \"not_attempted\",\n";
    json << "  \"keys_used\": false,\n";
    json << "  \"extracted\": " << (analysis.extracted ? "true" : "false") << ",\n";
    json << "  \"extracted_path\": \"" << JsonEscape(analysis.extractedPath) << "\",\n";
    json << "  \"extracted_size\": " << analysis.extractedSize << ",\n";
    json << "  \"missing_prerequisites\": [\"no per-file crypto keys available\", \"no crypto operation implemented or requested\", \"hardware secrets unavailable\"],\n";
    json << "  \"tool\": \"self_inspector (analysis and verified reassembly only; not a decryptor)\"\n";
    json << "}\n";
    return json.str();
}

int Run(const Options& options, Analysis& analysis) {
    std::vector<std::uint8_t> data;
    std::string error;
    if (!ReadFile(options.inputPath, data, error)) {
        analysis.error = error;
        return 2;
    }
    analysis = AnalyzeBuffer(data);
    if (!analysis.ok) return 2;
    if (!options.extractPath.empty()) {
        std::vector<std::uint8_t> reassembled;
        if (!ReassembleElf(data, analysis, reassembled, error)) {
            analysis.error = error;
            return 2;
        }
        if (!WriteFile(options.extractPath, reassembled, error)) {
            analysis.error = error;
            return 2;
        }
        analysis.extracted = true;
        analysis.extractedPath = options.extractPath;
        analysis.extractedSize = reassembled.size();
    }
    if (!options.reportPath.empty()) {
        const std::string text = BuildJson(analysis, options.inputPath);
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        if (!WriteFile(options.reportPath, bytes, error)) {
            analysis.error = error;
            return 2;
        }
    }
    return 0;
}

} // namespace SelfInspector
