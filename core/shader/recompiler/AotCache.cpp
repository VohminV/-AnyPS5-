#include "AotCache.hpp"

#include "ShaderCacheDirectory.hpp"
#include "ShaderDiskCache.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <thread>

namespace ShaderRecompiler {

// Запускает re-spirv (или spirv-opt) над сгенерированным SPIR-V и возвращает
// оптимизированные слова. Если утилита не найдена — возвращает исходник без
// изменений (кэш всё равно работает, просто без сжатия).
static std::vector<std::uint32_t> OptimizeSpirv(std::span<const std::uint32_t> spv) {
    std::error_code error;
    auto tmpDir = std::filesystem::temp_directory_path(error);
    if (error) return {spv.begin(), spv.end()};
    auto inPath = tmpDir / ("aot_in_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".spv");
    auto outPath = tmpDir / ("aot_out_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".spv");
    {
        std::ofstream file(inPath, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(spv.data()), static_cast<std::streamsize>(spv.size() * sizeof(std::uint32_t)));
    }
    std::string cmd = "re-spirv -O --eliminate-dead-code-aggressive " + inPath.string() + " -o " + outPath.string() + " 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(inPath, error);
        return {spv.begin(), spv.end()};
    }
    std::vector<std::uint32_t> result;
    std::ifstream file(outPath, std::ios::binary | std::ios::ate);
    if (file) {
        const auto size = static_cast<std::streamoff>(file.tellg());
        if (size > 0 && size % sizeof(std::uint32_t) == 0) {
            file.seekg(0);
            result.resize(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
            file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size() * sizeof(std::uint32_t)));
        }
    }
    std::filesystem::remove(inPath, error);
    std::filesystem::remove(outPath, error);
    if (result.empty()) return {spv.begin(), spv.end()};
    if (result[0] != 0x07230203u) return {spv.begin(), spv.end()};
    return result;
}

std::uint64_t AotCache::HashCode(std::span<const std::uint32_t> code) {
    const auto bytes = std::as_bytes(code);
    return HashBytes(bytes, 0xa0761d5478c3d96full);
}

std::uint64_t AotCache::HashCode(std::span<const std::uint32_t> code, const BindingLayout& layout) {
    auto hash = HashCode(code);
    hash ^= static_cast<std::uint64_t>(layout.descriptorSet) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= static_cast<std::uint64_t>(layout.firstBinding) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= static_cast<std::uint64_t>(layout.pushConstantOffsetBytes / 4u) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= static_cast<std::uint64_t>(layout.pushConstantSizeBytes / 4u) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

std::string AotCache::EntryName(std::uint64_t hash) {
    char buf[17];
    for (int i = 0; i < 8; ++i) {
        const auto nibble = static_cast<unsigned>((hash >> ((7 - i) * 4)) & 0xfull);
        buf[i] = char(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    buf[8] = '\0';
    return std::string(buf) + ".avc";
}

std::filesystem::path AotCache::Directory() {
    const char* directory = std::getenv("ANYPS5_AOT_CACHE_DIR");
    if (directory != nullptr && *directory != '\0') return std::filesystem::path(directory);
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) return executable.parent_path() / "aot_shader_cache";
    return std::filesystem::current_path() / "aot_shader_cache";
}

bool AotCache::Enabled() {
    const char* disabled = std::getenv("APS5_NO_AOT_CACHE");
    return disabled == nullptr || std::strcmp(disabled, "0") == 0;
}

std::optional<CompiledVariant> AotCache::Lookup(std::uint64_t hash) {
    if (!Enabled()) return std::nullopt;
    const auto dir = Directory();
    if (dir.empty()) return std::nullopt;
    const auto path = dir / EntryName(hash);
    std::vector<std::byte> file;
    if (!ReadWholeFile(path, file)) return std::nullopt;
    std::array<std::byte, 8> key{};
    for (int i = 0; i < 8; ++i) key[i] = static_cast<std::byte>((hash >> (i * 8)) & 0xffu);
    CompiledVariant variant;
    if (ShaderDiskCache::DecodeEntry(file, key, variant) != ShaderDiskCache::LoadStatus::Loaded)
        return std::nullopt;
    return variant;
}

void AotCache::Store(std::uint64_t hash, const CompiledVariant& variant) {
    if (!Enabled()) return;
    const auto dir = Directory();
    if (dir.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::array<std::byte, 8> key{};
    for (int i = 0; i < 8; ++i) key[i] = static_cast<std::byte>((hash >> (i * 8)) & 0xffu);
    // Сжимаем SPIR-V через re-spirv перед сохранением (940 КБ -> ~15 КБ).
    CompiledVariant optimized = variant;
    optimized.artifact.spirv = OptimizeSpirv(variant.artifact.spirv);
    const auto payload = ShaderDiskCache::EncodeEntry(key, optimized);
    const auto path = dir / EntryName(hash);
    if (!WriteFileAtomically(path, payload)) {
        std::fprintf(stderr, "[aot-cache] cannot write %s\n", path.string().c_str());
    }
}

} // namespace ShaderRecompiler
