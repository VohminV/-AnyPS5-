 #include "SelfInspector.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void PrintUsage() {
    std::cerr << "Usage: self_inspector <input> --report <report.json> [--extract <output.elf>]\n"
                 "Analyze a PS5 SELF container read-only and write a JSON diagnostic report.\n"
                 "With --extract, slice the first valid embedded ELF (no crypto performed).\n";
}

SelfInspector::Options ParseArgs(int argc, char* argv[], std::string& error) {
    SelfInspector::Options options;
    std::vector<std::string> positionals;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--report" || arg == "--extract") {
            if (i + 1 >= argc) {
                error = "missing value for " + arg;
                return options;
            }
            if (arg == "--report") {
                if (!options.reportPath.empty()) {
                    error = "duplicate --report";
                    return options;
                }
                options.reportPath = argv[i + 1];
            } else {
                if (!options.extractPath.empty()) {
                    error = "duplicate --extract";
                    return options;
                }
                options.extractPath = argv[i + 1];
            }
            ++i;
        } else if (arg == "--help" || arg == "-h") {
            error = "help";
            return options;
        } else if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
            error = "unknown option: " + arg;
            return options;
        } else {
            positionals.push_back(arg);
        }
    }
    if (positionals.size() != 1) {
        error = "exactly one input file is required";
        return options;
    }
    if (options.reportPath.empty()) {
        error = "--report <report.json> is required";
        return options;
    }
    options.inputPath = positionals[0];
    return options;
}

} // namespace

int main(int argc, char* argv[]) {
    std::string error;
    const SelfInspector::Options options = ParseArgs(argc, argv, error);
    if (!error.empty()) {
        if (error != "help") std::cerr << "FAIL: " << error << "\n";
        PrintUsage();
        return 1;
    }

    SelfInspector::Analysis analysis;
    const int code = SelfInspector::Run(options, analysis);
    if (code != 0) {
        std::cerr << "FAIL: " << analysis.error << "\n";
        std::cerr << "Input: " << options.inputPath << "\n";
        return code;
    }
    std::cout << "Container: " << SelfInspector::ContainerKindName(analysis.container) << "\n";
    std::cout << "File size: " << analysis.fileSize << "\n";
    std::cout << "Segment records: " << analysis.records.size() << "\n";
    std::cout << "Embedded ELFs: " << analysis.elfs.size() << "\n";
    for (const auto& elf : analysis.elfs) {
        std::cout << "ELF at 0x" << std::hex << elf.fileOffset << std::dec
                  << " type=0x" << std::hex << elf.type << " machine=0x" << elf.machine << std::dec
                  << " phnum=" << elf.progHeaderCount
                  << " span=0x" << std::hex << elf.span << std::dec
                  << " valid=" << (elf.headersValid ? "yes" : "no") << "\n";
    }
    std::cout << "Decryption: not attempted, keys not used\n";
    for (const auto& m : analysis.mapping) {
        std::cout << "Map phdr " << m.phdrIndex << " <- record " << m.recordIndex
                  << " self=0x" << std::hex << m.selfOffset
                  << " elf=0x" << m.elfOffset
                  << " size=0x" << m.size << std::dec << "\n";
    }
    if (analysis.extracted) {
        std::cout << "Extracted: " << analysis.extractedPath
                  << " (" << analysis.extractedSize << " bytes)\n";
    }
    std::cout << "Report: " << options.reportPath << "\n";
    return 0;
}
