#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include "ShaderCacheDirectory.hpp"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace AgcDriver::DriverDetail {

namespace {

// Dumps (shader_fail_*.bin, shader_slow_*.bin, shader_*.req) go next to the
// disk cache so `ANYPS5_SHADER_CACHE_DIR=<game>/shader` keeps them in one place
// and a rerun can reload compiled artifacts. Falls back to the previous CWD
// behavior when no cache directory is configured.
std::filesystem::path ShaderDumpDirectory() {
    if (const char* override = std::getenv("APS5_SHADER_DUMP_DIR")) {
        if (*override != '\0') return std::filesystem::path(override);
    }
    const auto cache = ShaderRecompiler::ShaderCacheDirectory();
    if (!cache.empty()) return cache;
    return {};
}

std::string DumpPath(const char* fileName) {
    const auto directory = ShaderDumpDirectory();
    if (directory.empty()) return std::string(fileName);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return (directory / fileName).string();
}

}

void Driver::reportSkip(const char* kind, const std::string& what) {
    static std::mutex reportedMutex;
    static std::set<std::size_t> reported;
    static std::map<std::string, int> requestDumps;

    static const bool traceSkips = std::getenv("APS5_TRACE_SKIPS") != nullptr;
    static const int dumpLimit = [] { const char* text = std::getenv("APS5_SKIP_REQUEST_DUMPS"); return text ? std::atoi(text) : 8; }();
    static constexpr std::string_view marker = "\nRecompileRequest:\n";
    std::lock_guard lock(reportedMutex);
    std::string line;
    static bool announced = false;
    if (!announced) {
        announced = true;
        char text[160];
        std::snprintf(text, sizeof(text), "[gpu] skip reports: one write each, RecompileRequest dumps capped at %d per reason (APS5_SKIP_REQUEST_DUMPS)\n", dumpLimit);
        line = text;
    }
    const std::string prefix = "[gpu] skipped " + std::string(kind);
    if (reported.insert(std::hash<std::string>{}(what)).second) {
        const auto request = what.find(marker);
        if (request == std::string::npos) {
            line += prefix + ": " + what + "\n";
        } else {
            const auto reason = what.substr(0, request);
            const auto suffix = what.find(' ', request + marker.size());
            auto& dumps = requestDumps[reason];
            if (dumpLimit < 0 || dumps < dumpLimit) {
                ++dumps;
                line += prefix + ": " + what + "\n";
            } else {
                if (dumps == dumpLimit) {
                    ++dumps;
                    line += prefix + ": further RecompileRequest dumps for \"" + reason + "\" are left out\n";
                }
                line += prefix + ": " + reason + " (request left out)" + (suffix == std::string::npos ? std::string() : what.substr(suffix)) + "\n";
            }
        }
    } else if (traceSkips) {
        line += prefix + " again: " + what.substr(0, 100) + "\n";
    }
    if (!line.empty()) std::fwrite(line.data(), 1, line.size(), stderr);
}

std::string Driver::dumpRequest(std::uint64_t address, const ShaderRecompiler::RecompileRequest& request) {
    static std::mutex dumpMutex;
    static std::set<std::uint64_t> dumped;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_%llx.req", static_cast<unsigned long long>(address));
    const std::string path = DumpPath(name);
    std::lock_guard lock(dumpMutex);
    if (!dumped.insert(address).second) return path;
    try {
        const auto text = ShaderRecompiler::RequestSerializer{}.Serialize(request);
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[gpu] could not serialize request for 0x%llx: %s\n", static_cast<unsigned long long>(address), error.what());
    }
    return path;
}

void DumpFailedShaderCode(std::uint64_t address, std::span<const std::uint32_t> code) {
    static const bool dump = std::getenv("APS5_DUMP_FAILED_SHADERS") != nullptr;
    if (!dump || code.empty()) return;
    static std::mutex dumpMutex;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_fail_%llx.bin", static_cast<unsigned long long>(address));
    const std::string path = DumpPath(name);
    std::lock_guard lock(dumpMutex);
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(code.data(), sizeof(std::uint32_t), code.size(), file);
        std::fclose(file);
    }
}

void DumpFailedShaderRequest(std::uint64_t address, const std::string& serialized) {
    static const bool dump = std::getenv("APS5_DUMP_FAILED_SHADERS") != nullptr;
    if (!dump || serialized.empty()) return;
    static std::mutex dumpMutex;
    static std::set<std::uint64_t> dumped;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_fail_%llx.req", static_cast<unsigned long long>(address));
    const std::string path = DumpPath(name);
    std::lock_guard lock(dumpMutex);
    if (!dumped.insert(address).second) return;
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(serialized.data(), 1, serialized.size(), file);
        std::fclose(file);
    }
}

PrepareWatchdog::PrepareWatchdog(std::uint64_t address, const char* site, std::span<const std::uint32_t> code)
    : address_(address), site_(site), code_(code), start_(std::chrono::steady_clock::now()) {
    std::fprintf(stderr, "[shader] prepare start 0x%llx (%s) words=%zu\n", static_cast<unsigned long long>(address_), site_, code_.size());
}

PrepareWatchdog::~PrepareWatchdog() {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    if (seconds < 30.0) return;
    std::fprintf(stderr, "[shader] prepare slow 0x%llx (%s) %.1fs\n", static_cast<unsigned long long>(address_), site_, seconds);
    static std::mutex dumpMutex;
    static std::set<std::uint64_t> dumped;
    std::lock_guard lock(dumpMutex);
    if (!dumped.insert(address_).second || code_.empty()) return;
    char name[64];
    std::snprintf(name, sizeof(name), "shader_slow_%llx.bin", static_cast<unsigned long long>(address_));
    const std::string path = DumpPath(name);
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(code_.data(), sizeof(std::uint32_t), code_.size(), file);
        std::fclose(file);
    }
}

}
