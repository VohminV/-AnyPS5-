#include "CpuBackend/Simd.hpp"
#include <immintrin.h>

namespace AgcDriver::CpuBackend {

std::uint64_t Hash64Avx2(const std::uint32_t* words, std::size_t count, std::uint64_t seed) {
    std::uint64_t hash = 1469598103934665603ull ^ (seed * 1099511628211ull);
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{7};
    for (; i < blocked; i += 8) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(words + i));
        alignas(32) std::uint32_t lanes[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), chunk);
        for (int k = 0; k < 8; ++k) {
            std::uint64_t v = static_cast<std::uint64_t>(lanes[k]) + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
            hash ^= v;
            hash *= 1099511628211ull;
        }
    }
    for (; i < count; ++i) {
        std::uint64_t v = static_cast<std::uint64_t>(words[i]) + 0x9e3779b97f4a7c15ull + (hash << 6ull) + (hash >> 2ull);
        hash ^= v;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(count) * 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 29ull;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 32ull;
    return hash;
}

bool EqualAvx2(const std::uint32_t* left, const std::uint32_t* right, std::size_t count) {
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{7};
    for (; i < blocked; i += 8) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + i));
        __m256i c = _mm256_cmpeq_epi32(a, b);
        int mask = _mm256_movemask_epi8(c);
        if (mask != static_cast<int>(0xFFFFFFFF)) {
            return false;
        }
    }
    for (; i < count; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

void CopyAvx2(std::uint32_t* out, const std::uint32_t* in, std::size_t count) {
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{7};
    for (; i < blocked; i += 8) {
        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i), chunk);
    }
    for (; i < count; ++i) {
        out[i] = in[i];
    }
}

} // namespace AgcDriver::CpuBackend
