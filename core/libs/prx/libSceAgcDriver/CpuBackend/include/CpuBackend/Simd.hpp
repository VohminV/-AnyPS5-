#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::CpuBackend {

struct Features {
    bool sse2 = false;
    bool sse41 = false;
    bool avx = false;
    bool avx2 = false;
    bool osYmm = false;
};

enum class Path : std::uint8_t {
    Scalar = 0,
    Sse = 1,
    Avx2 = 2
};

Features Probe();
Path Select(Features features);
const char* Name(Path path);

std::uint64_t Hash64Scalar(const std::uint32_t* words, std::size_t count, std::uint64_t seed);
std::uint64_t Hash64Sse(const std::uint32_t* words, std::size_t count, std::uint64_t seed);
std::uint64_t Hash64Avx2(const std::uint32_t* words, std::size_t count, std::uint64_t seed);
std::uint64_t Hash64(const std::uint32_t* words, std::size_t count, std::uint64_t seed);
std::uint64_t Hash64Dispatch(const std::uint32_t* words, std::size_t count, std::uint64_t seed, Path path);

bool EqualScalar(const std::uint32_t* left, const std::uint32_t* right, std::size_t count);
bool EqualSse(const std::uint32_t* left, const std::uint32_t* right, std::size_t count);
bool EqualAvx2(const std::uint32_t* left, const std::uint32_t* right, std::size_t count);
bool EqualDispatch(const std::uint32_t* left, const std::uint32_t* right, std::size_t count, Path path);

void CopyScalar(std::uint32_t* out, const std::uint32_t* in, std::size_t count);
void CopySse(std::uint32_t* out, const std::uint32_t* in, std::size_t count);
void CopyAvx2(std::uint32_t* out, const std::uint32_t* in, std::size_t count);
void CopyDispatch(std::uint32_t* out, const std::uint32_t* in, std::size_t count, Path path);

std::uint64_t FoldSeed(std::uint64_t base, std::uint64_t mix);

} // namespace AgcDriver::CpuBackend
