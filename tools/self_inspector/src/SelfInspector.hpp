 #ifndef SELF_INSPECTOR_HPP
#define SELF_INSPECTOR_HPP

// self_inspector: read-only diagnostic analyzer for PS5 SELF containers.
//
// Scope: container analysis and verified reassembly of a stored-open payload.
// No cryptography is implemented, requested, or performed by this tool.
// Field semantics below are OBSERVED on real files and cross-checked with
// open community sources (see SELF_ANALYSIS.md); they are not official
// Sony documentation. Anything unconfirmed is reported as unconfirmed.

#include <cstdint>
#include <string>
#include <vector>

namespace SelfInspector {

enum class ContainerKind {
    TooSmall,
    PlainElf,
    SelfApp,    // magic 0x1D3D154F ("O\x15=\x1d"), relinker test name: eboot.self
    SelfPkg,    // magic 0xEEF51454 ("T\x14\xf5\xee"), relinker test name: eboot.self.kernel
    Pkg,        // magic bytes "7FCNT"
    Unknown
};

const char* ContainerKindName(ContainerKind kind);

struct SelfRecord {
    std::uint64_t word0 = 0;
    std::uint64_t word1 = 0; // data records: SELF file offset of the segment bytes
    std::uint64_t word2 = 0;
    std::uint64_t word3 = 0;
    bool pairEqual = false; // word2 == word3, observed on every record so far
    std::uint64_t id = 0;   // word0 >> 20; observed to equal the program header index
    bool dataSegment = false; // word0 & 0x800, per PS4 scene docs (UNCONFIRMED for PS5)
    bool encrypted = false;   // word0 & 0x2, per PS4 scene docs (UNCONFIRMED for PS5)
};

struct ProgHeader {
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint64_t offset = 0; // ELF-relative file offset
    std::uint64_t vaddr = 0;
    std::uint64_t filesz = 0;
    std::uint64_t memsz = 0;
};

struct EmbeddedElf {
    std::uint64_t fileOffset = 0; // offset of 7F 45 4C 46 in the input
    std::uint16_t type = 0;
    std::uint16_t machine = 0;
    std::uint64_t entry = 0;
    std::uint64_t progHeaderOffset = 0; // ELF-relative
    std::uint16_t progHeaderCount = 0;
    std::uint64_t sectHeaderOffset = 0; // reported only; often out of range (stripped)
    std::uint64_t span = 0;             // max(p_offset + p_filesz) over program headers
    bool headersValid = false;
    // For an embedded ELF, p_offset/p_filesz describe the OUTPUT layout, not
    // the container layout: ranges may exceed the input size until remapped.
    bool rangesVerified = false;
    std::vector<ProgHeader> phdrs;
    std::vector<std::string> issues;
};

struct SegmentMapping {
    int phdrIndex = -1;
    int recordIndex = -1;
    std::uint64_t selfOffset = 0;
    std::uint64_t elfOffset = 0;
    std::uint64_t size = 0;
};

struct Analysis {
    bool ok = false;
    std::string error; // set when ok == false
    ContainerKind container = ContainerKind::Unknown;
    std::uint32_t magic = 0;
    std::uint64_t fileSize = 0;
    // First 16 little-endian u32 words of the prologue (offsets 0x00..0x3C).
    // Meaning of most words is UNCONFIRMED; word[4] correlates with file size.
    std::uint32_t prologue[16] = {};
    bool hasPrologue = false;
    std::vector<SelfRecord> records;
    std::uint64_t recordsRemainder = 0; // trailing bytes in [0x40, elf) not forming a full record
    std::vector<EmbeddedElf> elfs;      // every 7F454C46 hit in the whole file (capped)
    std::uint64_t elfHitsCapped = 0;
    bool anyEncryptedRecord = false;
    bool anyRecordPairMismatch = false;
    double payloadSampleEntropy = -1.0; // -1 when not sampled
    // Reassembly (plaintext record remap; no crypto).
    std::vector<SegmentMapping> mapping;
    std::uint64_t headerPreserveEnd = 0;
    std::uint64_t passthroughBytes = 0;
    bool extracted = false;
    std::string extractedPath;
    std::uint64_t extractedSize = 0;
    bool keysUsed = false; // always false: this tool performs no crypto
};

struct Options {
    std::string inputPath;
    std::string reportPath;   // empty: no JSON written
    std::string extractPath;  // empty: no extraction attempted
};

// Full pipeline: read input, analyze, optionally extract, optionally report.
// Returns process exit code: 0 ok, 2 input/format/validation failure.
int Run(const Options& options, Analysis& analysis);

// Pure building blocks (used by tests).
bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out, std::string& error);
Analysis AnalyzeBuffer(const std::vector<std::uint8_t>& data);
bool ReassembleElf(const std::vector<std::uint8_t>& data, const Analysis& analysis,
                   std::vector<std::uint8_t>& out, std::string& error);
bool WriteFile(const std::string& path, const std::vector<std::uint8_t>& data, std::string& error);
std::string BuildJson(const Analysis& analysis, const std::string& inputPath);
double ShannonEntropy(const std::vector<std::uint8_t>& data, std::uint64_t offset, std::uint64_t size);

} // namespace SelfInspector

#endif
