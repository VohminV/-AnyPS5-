#include "CpuBackend/Simd.hpp"
#include <immintrin.h>
#include <cstring>

namespace AgcDriver::CpuBackend {

std::uint64_t Hash64Sse(const std::uint32_t* words, std::size_t count, std::uint64_t seed) {
    std::uint64_t hash = 1469598103934665603ull ^ (seed * 1099511628211ull);
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{3};
    for (; i < blocked; i += 4) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i));
        alignas(16) std::uint32_t lanes[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(lanes), chunk);
        for (int k = 0; k < 4; ++k) {
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

bool EqualSse(const std::uint32_t* left, const std::uint32_t* right, std::size_t count) {
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{3};
    for (; i < blocked; i += 4) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(left + i));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(right + i));
        __m128i c = _mm_cmpeq_epi32(a, b);
        int mask = _mm_movemask_epi8(c);
        if (mask != 0xFFFF) {
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

void CopySse(std::uint32_t* out, const std::uint32_t* in, std::size_t count) {
    std::size_t i = 0;
    std::size_t blocked = count & ~std::size_t{3};
    for (; i < blocked; i += 4) {
        __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + i), chunk);
    }
    for (; i < count; ++i) {
        out[i] = in[i];
    }
}

} // namespace AgcDriver::CpuBackend
