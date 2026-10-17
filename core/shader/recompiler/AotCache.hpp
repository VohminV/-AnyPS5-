#ifndef CORE_SHADER_RECOMPILER_AOTCACHE_HPP
#define CORE_SHADER_RECOMPILER_AOTCACHE_HPP

#include "CompiledVariant.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace ShaderRecompiler {

// Runtime AOT shader cache: keyed on the raw RDNA2 binary hash plus the static
// pipeline layout, so a shader compiled once is reused on every later launch
// without recompilation.
// Directory: ANYPS5_AOT_CACHE_DIR or <cwd>/aot_shader_cache/.
// Kill switch: APS5_NO_AOT_CACHE=1.
class AotCache {
public:
    [[nodiscard]] static bool Enabled();
    [[nodiscard]] static std::filesystem::path Directory();

    [[nodiscard]] static std::uint64_t HashCode(std::span<const std::uint32_t> code);
    [[nodiscard]] static std::uint64_t HashCode(std::span<const std::uint32_t> code, const BindingLayout& layout);

    [[nodiscard]] static std::optional<CompiledVariant> Lookup(std::uint64_t hash);

    static void Store(std::uint64_t hash, const CompiledVariant& variant);

private:
    [[nodiscard]] static std::string EntryName(std::uint64_t hash);
};

} // namespace ShaderRecompiler

#endif
