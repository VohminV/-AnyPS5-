#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "CpuBackend/Backend.hpp"

namespace AgcDriver::CpuBackend {

struct StreamCase {
    std::string name;
    std::vector<std::uint32_t> dwords;
};

struct CaseResult {
    std::string name;
    std::size_t dwords = 0;
    std::size_t packets = 0;
    double legacyUs = 0.0;
    double fastUs = 0.0;
    double perCommandNsLegacy = 0.0;
    double perCommandNsFast = 0.0;
    double speedup = 0.0;
    bool identical = true;
    std::uint64_t legacyDigest = 0;
    std::uint64_t fastDigest = 0;
    std::uint64_t legacyDraws = 0;
    std::uint64_t fastDraws = 0;
    PhaseSnapshot legacyPhases{};
    PhaseSnapshot fastPhases{};
    std::uint64_t fastHits = 0;
    std::uint64_t fastMisses = 0;
};

struct BenchReport {
    std::vector<CaseResult> cases;
    double totalLegacyUs = 0.0;
    double totalFastUs = 0.0;
    double totalSpeedup = 0.0;
    bool allIdentical = true;
};

std::vector<StreamCase> MakeStreamCases(std::uint32_t seed);
BenchReport RunBench(const std::vector<StreamCase>& cases, std::size_t repeats);
std::string FormatReport(const BenchReport& report);

} // namespace AgcDriver::CpuBackend
