 #include "SelfInspector.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* name) {
    if (condition) {
        std::printf("PASS: %s\n", name);
    } else {
        std::printf("FAIL: %s\n", name);
        ++failures;
    }
}

void PutU64(std::vector<std::uint8_t>& data, std::uint64_t off, std::uint64_t value) {
    std::memcpy(data.data() + off, &value, 8);
}

// Synthetic SELF under the remap model: one PT_LOAD (phdr 0) whose bytes live
// at a SELF record offset different from the ELF target offset.
std::vector<std::uint8_t> BuildSyntheticSelf(std::uint64_t payloadSize, bool encrypted) {
    std::vector<std::uint8_t> data(0x40, 0);
    data[0] = 0x54; data[1] = 0x14; data[2] = 0xF5; data[3] = 0xEE;
    data[4] = 0x00; data[5] = 0x01; data[6] = 0x01; data[7] = 0x12;
    const std::uint64_t elfBase = 0x40 + 32;
    const std::uint64_t loadTarget = 0x1000;
    const std::uint64_t span = loadTarget + payloadSize + 0x100; // room for PT_DYNAMIC
    data.resize(elfBase + span + 0x2000, 0);
    // Record 0: id 0, data segment, optionally encrypted flag.
    PutU64(data, 0x40, encrypted ? 0x806ull : 0x804ull);
    const std::uint64_t payloadSelfOff = elfBase + span + 0x100;
    PutU64(data, 0x48, payloadSelfOff);
    PutU64(data, 0x50, payloadSize);
    PutU64(data, 0x58, payloadSize);
    // Embedded ELF.
    std::uint8_t* elf = data.data() + elfBase;
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[6] = 1;
    elf[16] = 0x10; elf[17] = 0xFE;
    elf[18] = 0x3E;
    elf[20] = 1;
    elf[52] = 64;
    elf[32] = 64; // phoff
    elf[54] = 56; elf[56] = 2; // phentsize, phnum
    elf[58] = 64; elf[60] = 0;
    // phdr 0: PT_LOAD -> loadTarget.
    std::uint8_t* ph0 = elf + 64;
    ph0[0] = 1;
    PutU64(data, elfBase + 64 + 8, loadTarget);
    PutU64(data, elfBase + 64 + 32, payloadSize);
    PutU64(data, elfBase + 64 + 40, payloadSize);
    // phdr 1: PT_DYNAMIC inside the LOAD range.
    const std::uint64_t dynOff = loadTarget + payloadSize;
    std::uint8_t* ph1 = elf + 64 + 56;
    ph1[0] = 2;
    PutU64(data, elfBase + 64 + 56 + 8, dynOff);
    PutU64(data, elfBase + 64 + 56 + 32, 32);
    PutU64(data, elfBase + 64 + 56 + 40, 32);
    // Payload bytes at the SELF record offset (proves remap, not slicing).
    for (std::uint64_t i = 0; i < payloadSize; ++i) data[payloadSelfOff + i] = static_cast<std::uint8_t>((i * 7 + 1) & 0xFF);
    // PT_DYNAMIC content at its remapped home: DT_NEEDED + DT_NULL.
    const std::uint64_t dynSelf = payloadSelfOff + payloadSize;
    PutU64(data, dynSelf, 1);
    PutU64(data, dynSelf + 8, 0);
    PutU64(data, dynSelf + 16, 0);
    PutU64(data, dynSelf + 24, 0);
    // Extend the LOAD to cover the dynamic so mapping by size still holds:
    // (payload + 32 dynamic bytes travel together).
    return data;
}

std::string TempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace

int main() {
    {
        // NOTE: dynamic travels inside the LOAD in this synthetic; adjust the
        // LOAD size to payload+32 so the size-match rule holds exactly.
        std::vector<std::uint8_t> self = BuildSyntheticSelf(1024, false);
        const std::uint64_t elfBase = 0x40 + 32;
        const std::uint64_t payloadSize = 1024 + 32;
        PutU64(self, elfBase + 64 + 32, payloadSize);
        PutU64(self, elfBase + 64 + 40, payloadSize);
        PutU64(self, 0x50, payloadSize);
        PutU64(self, 0x58, payloadSize);
        for (std::uint64_t i = 1024; i < payloadSize; ++i)
            self[elfBase + 0x100 + 0x1000 + i] = 0;
        std::string error;
        const std::string path = TempPath("self_inspector_valid.self");
        Check(SelfInspector::WriteFile(path, self, error), "write synthetic SELF");
        SelfInspector::Options options;
        options.inputPath = path;
        options.reportPath = TempPath("self_inspector_valid.json");
        options.extractPath = TempPath("self_inspector_valid.elf");
        SelfInspector::Analysis analysis;
        Check(SelfInspector::Run(options, analysis) == 0, "valid SELF exit 0");
        Check(analysis.container == SelfInspector::ContainerKind::SelfPkg, "container SELF");
        Check(analysis.records.size() == 1, "1 record parsed");
        Check(!analysis.anyEncryptedRecord, "no encrypted record");
        Check(analysis.elfs.size() == 1 && analysis.elfs.front().headersValid, "one valid embedded ELF");
        Check(analysis.mapping.size() == 1, "one segment mapped");
        Check(analysis.extracted && analysis.extractedSize == 0x1000 + payloadSize, "ELF reassembled with exact span");
        Check(!analysis.keysUsed, "keys never used");
        std::vector<std::uint8_t> back;
        Check(SelfInspector::ReadFile(options.extractPath, back, error), "read back reassembled ELF");
        bool placed = true;
        for (std::uint64_t i = 0; i < 1024; ++i) {
            if (back[0x1000 + i] != static_cast<std::uint8_t>((i * 7 + 1) & 0xFF)) {
                placed = false;
                break;
            }
        }
        Check(placed, "payload remapped to ELF target, not sliced");
        Check(back[0] == 0x7F && back[1] == 'E', "ELF magic at slice start");
        std::vector<std::uint8_t> report;
        Check(SelfInspector::ReadFile(options.reportPath, report, error) && !report.empty(), "report written");
        const std::string reportText(report.begin(), report.end());
        Check(reportText.find("\"phnum\": 2,") != std::string::npos, "report has numeric phnum without stray quote");
        Check(reportText.find("\"keys_used\": false") != std::string::npos, "report states keys not used");
    }
    {
        const std::vector<std::uint8_t> self = BuildSyntheticSelf(64, true);
        const std::string path = TempPath("self_inspector_enc.self");
        std::string error;
        Check(SelfInspector::WriteFile(path, self, error), "write encrypted-flag SELF");
        const SelfInspector::Analysis analysis = SelfInspector::AnalyzeBuffer(self);
        Check(analysis.anyEncryptedRecord, "encrypted flag detected");
        std::vector<std::uint8_t> out;
        Check(!SelfInspector::ReassembleElf(self, analysis, out, error), "reassembly refused on encrypted flag");
    }
    {
        const std::vector<std::uint8_t> self = BuildSyntheticSelf(1024, false);
        const std::string path = TempPath("self_inspector_trunc.self");
        std::string error;
        const std::vector<std::uint8_t> cut(self.begin(), self.begin() + 0x50);
        Check(SelfInspector::WriteFile(path, cut, error), "write truncated SELF");
        SelfInspector::Options options;
        options.inputPath = path;
        options.reportPath = TempPath("self_inspector_trunc.json");
        SelfInspector::Analysis analysis;
        Check(SelfInspector::Run(options, analysis) == 2, "truncated SELF exit 2");
        Check(!analysis.error.empty(), "truncated SELF has reason");
    }
    {
        const std::string path = TempPath("self_inspector_bad.self");
        const std::vector<std::uint8_t> bad = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
        std::string error;
        Check(SelfInspector::WriteFile(path, bad, error), "write bad-magic file");
        SelfInspector::Options options;
        options.inputPath = path;
        options.reportPath = TempPath("self_inspector_bad.json");
        SelfInspector::Analysis analysis;
        Check(SelfInspector::Run(options, analysis) == 2, "bad magic exit 2");
    }
    {
        std::vector<std::uint8_t> self = BuildSyntheticSelf(64, false);
        const std::uint64_t elfBase = 0x40 + 32;
        const std::uint64_t huge = 0xFFFFFFFFFFFFull;
        std::memcpy(self.data() + elfBase + 64 + 32, &huge, 8); // p_filesz far past EOF
        const SelfInspector::Analysis analysis = SelfInspector::AnalyzeBuffer(self);
        // Embedded ranges describe the OUTPUT layout, so parsing alone stays
        // valid; reassembly must refuse on the record size mismatch instead.
        Check(!analysis.elfs.empty() && analysis.elfs.front().headersValid, "OOB range parsed without input-bound failure");
        std::vector<std::uint8_t> out;
        std::string error;
        Check(!SelfInspector::ReassembleElf(self, analysis, out, error), "reassembly refused");
    }
    {
        const std::string path = TempPath("self_inspector_tiny.bin");
        const std::vector<std::uint8_t> tiny = {0x7F, 0x45, 0x4C};
        std::string error;
        Check(SelfInspector::WriteFile(path, tiny, error), "write tiny file");
        SelfInspector::Options options;
        options.inputPath = path;
        options.reportPath = TempPath("self_inspector_tiny.json");
        SelfInspector::Analysis analysis;
        Check(SelfInspector::Run(options, analysis) == 2, "tiny file exit 2");
    }
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("All self_inspector tests passed\n");
    return 0;
}
