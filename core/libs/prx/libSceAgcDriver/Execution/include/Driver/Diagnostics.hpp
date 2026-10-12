#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DIAGNOSTICS_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace AgcDriver::DriverDetail {

double TraceMs();

void require(bool condition, const char* reason);

// Writes the failing shader's code words to shader_fail_<address>.bin when
// APS5_DUMP_FAILED_SHADERS is set, so a fatal translation error can be
// replayed offline. Bounded: one file per address, only on failure.
void DumpFailedShaderCode(std::uint64_t address, std::span<const std::uint32_t> code);

// Traces one shader preparation: logs the start always (so a hang names the
// shader), and on slow completion (>30 s) logs the duration and dumps the
// code for offline replay. Bounded: one start line per preparation, dumps
// deduplicated by address.
class PrepareWatchdog {
public:
    PrepareWatchdog(std::uint64_t address, const char* site, std::span<const std::uint32_t> code);
    ~PrepareWatchdog();

private:
    std::uint64_t address_;
    const char* site_;
    std::span<const std::uint32_t> code_;
    std::chrono::steady_clock::time_point start_;
};

// Writes an already-serialized recompile request to
// shader_fail_<address>.req under the same switch. Bounded the same way.
void DumpFailedShaderRequest(std::uint64_t address, const std::string& serialized);

std::uint32_t readRegister(const Registers& registers, std::uint32_t offset);

std::uint32_t readUserData(const Registers& shader, std::uint32_t offset);

}

#endif
