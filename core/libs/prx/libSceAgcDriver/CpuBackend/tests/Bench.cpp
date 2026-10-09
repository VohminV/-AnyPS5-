#include "CpuBackend/Bench.hpp"
#include <cstdio>
#include <stdexcept>

int main() {
    auto cases = AgcDriver::CpuBackend::MakeStreamCases(12345u);
    if (cases.size() != 5) {
        throw std::runtime_error("bench cases");
    }
    for (const auto& item : cases) {
        if (item.dwords.empty()) {
            throw std::runtime_error("empty case");
        }
    }
    AgcDriver::CpuBackend::BenchReport report = AgcDriver::CpuBackend::RunBench(cases, 20);
    std::string text = AgcDriver::CpuBackend::FormatReport(report);
    std::fputs(text.c_str(), stdout);
    if (!report.allIdentical) {
        throw std::runtime_error("bench identical");
    }
    if (report.cases.empty()) {
        throw std::runtime_error("no cases");
    }
    std::puts("cpu_backend_bench ok");
    return 0;
}
