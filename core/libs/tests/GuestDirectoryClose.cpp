#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelGetdents(int, char*, int);
int APS5_VABI close_nid_postfix(int);
int APS5_VABI _close_nid_postfix(int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Directory close check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static constexpr int ErrorEnotdir = static_cast<int>(0x80020014u);
static constexpr int ErrorEnoent = static_cast<int>(0x80020002u);

static void VerifyCloseReuse(const std::filesystem::path& root, const char* scenario,
                             bool useUnderscoreClose) {
    const auto directoryPath = root / scenario;
    Require(std::filesystem::create_directory(directoryPath));
    const auto filePath = directoryPath / "entry.txt";
    {
        std::ofstream file(filePath, std::ios::binary);
        Require(file.is_open());
        file << "entry";
        Require(static_cast<bool>(file));
    }

    const int directory = sceKernelOpen(directoryPath.string().c_str(),
        SCE_KERNEL_O_RDONLY | SCE_KERNEL_O_DIRECTORY, 0);
    Require(directory >= 0);
    Require((useUnderscoreClose ? _close_nid_postfix(directory) : close_nid_postfix(directory)) == 0);

    const int file = sceKernelOpen(filePath.string().c_str(), SCE_KERNEL_O_RDONLY, 0);
    Require(file >= 0);
    Require(file == directory);

    std::array<char, 256> entries{};
    Require(sceKernelGetdents(file, entries.data(), static_cast<int>(entries.size())) == ErrorEnotdir);
    Require((useUnderscoreClose ? _close_nid_postfix(file) : close_nid_postfix(file)) == 0);
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-directory-close-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));

    VerifyCloseReuse(root, "close", false);
    VerifyCloseReuse(root, "underscore-close", true);

    const std::string slashed = (root / "close").string() + "/";
    const int directory = sceKernelOpen(slashed.c_str(),
        SCE_KERNEL_O_RDONLY | SCE_KERNEL_O_DIRECTORY, 0);
    Require(directory >= 0);
    std::array<char, 256> entries{};
    std::string names;
    for (;;) {
        const int read = sceKernelGetdents(directory, entries.data(), static_cast<int>(entries.size()));
        Require(read >= 0);
        if (read == 0) break;
        int offset = 0;
        while (offset < read) {
            std::uint16_t recordLength = 0;
            std::memcpy(&recordLength, entries.data() + offset + 4, sizeof(recordLength));
            Require(recordLength > 8 && offset + recordLength <= read);
            const auto nameLength = static_cast<std::uint8_t>(entries[offset + 7]);
            names += std::string(entries.data() + offset + 8, nameLength) + '\n';
            offset += recordLength;
        }
    }
    Require(names.find(".\n") != std::string::npos);
    Require(names.find("entry.txt\n") != std::string::npos);
    Require(close_nid_postfix(directory) == 0);

    Require(sceKernelOpen("", SCE_KERNEL_O_RDONLY, 0) == ErrorEnoent);

    std::filesystem::remove_all(root);
}
