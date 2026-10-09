#include "CpuBackend/Simd.hpp"
#if defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#else
#include <intrin.h>
#endif
#include <cstdint>

namespace AgcDriver::CpuBackend {

namespace {
bool OsYmmEnabled() {
#if defined(_MSC_VER)
    unsigned long long xcr = _xgetbv(0);
    return (xcr & 0x6ull) == 0x6ull;
#elif defined(__GNUC__) || defined(__clang__)
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    __get_cpuid_count(1, 0, &a, &b, &c, &d);
    bool osxsave = (c & (1u << 27u)) != 0u;
    bool avxBit = (c & (1u << 28u)) != 0u;
    if (!osxsave || !avxBit) {
        return false;
    }
    unsigned int lo = 0;
    unsigned int hi = 0;
#if defined(__x86_64__) || defined(_M_X64)
    __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    unsigned long long xcr = (static_cast<unsigned long long>(hi) << 32ull) | lo;
    return (xcr & 0x6ull) == 0x6ull;
#else
    return false;
#endif
#else
    return false;
#endif
}
} // namespace

Features Probe() {
    Features out{};
#if defined(__GNUC__) || defined(__clang__)
    unsigned int a = 0;
    unsigned int b = 0;
    unsigned int c = 0;
    unsigned int d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d) != 0) {
        out.sse2 = (d & (1u << 26u)) != 0u;
        out.sse41 = (c & (1u << 19u)) != 0u;
        out.avx = (c & (1u << 28u)) != 0u;
    }
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d) != 0) {
        out.avx2 = (b & (1u << 5u)) != 0u;
    }
    out.osYmm = OsYmmEnabled();
    if (!out.osYmm) {
        out.avx = false;
        out.avx2 = false;
    }
#if defined(__x86_64__)
    out.sse2 = true;
#endif
#else
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 1);
    out.sse2 = (info[3] & (1 << 26)) != 0;
    out.sse41 = (info[2] & (1 << 19)) != 0;
    out.avx = (info[2] & (1 << 28)) != 0;
    int sub[4] = {0, 0, 0, 0};
    __cpuidex(sub, 7, 0);
    out.avx2 = (sub[1] & (1 << 5)) != 0;
    out.osYmm = OsYmmEnabled();
    if (!out.osYmm) {
        out.avx = false;
        out.avx2 = false;
    }
    out.sse2 = true;
#endif
    return out;
}

Path Select(Features features) {
    if (features.avx2 && features.avx && features.osYmm) {
        return Path::Avx2;
    }
    if (features.sse41 || features.sse2) {
        return Path::Sse;
    }
    return Path::Scalar;
}

const char* Name(Path path) {
    if (path == Path::Avx2) {
        return "avx2";
    }
    if (path == Path::Sse) {
        return "sse";
    }
    return "scalar";
}

std::uint64_t FoldSeed(std::uint64_t base, std::uint64_t mix) {
    std::uint64_t v = base + 0x9e3779b97f4a7c15ull + (mix << 6ull) + (mix >> 2ull);
    v ^= v >> 29ull;
    v *= 0xbf58476d1ce4e5b9ull;
    v ^= v >> 32ull;
    return v;
}

std::uint64_t Hash64Scalar(const std::uint32_t* words, std::size_t count, std::uint64_t seed) {
    std::uint64_t hash = 1469598103934665603ull ^ (seed * 1099511628211ull);
    for (std::size_t i = 0; i < count; ++i) {
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

bool EqualScalar(const std::uint32_t* left, const std::uint32_t* right, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

void CopyScalar(std::uint32_t* out, const std::uint32_t* in, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = in[i];
    }
}

std::uint64_t Hash64(const std::uint32_t* words, std::size_t count, std::uint64_t seed) {
    return Hash64Scalar(words, count, seed);
}

std::uint64_t Hash64Dispatch(const std::uint32_t* words, std::size_t count, std::uint64_t seed, Path path) {
    if (path == Path::Avx2) {
        return Hash64Avx2(words, count, seed);
    }
    if (path == Path::Sse) {
        return Hash64Sse(words, count, seed);
    }
    return Hash64Scalar(words, count, seed);
}

bool EqualDispatch(const std::uint32_t* left, const std::uint32_t* right, std::size_t count, Path path) {
    if (path == Path::Avx2) {
        return EqualAvx2(left, right, count);
    }
    if (path == Path::Sse) {
        return EqualSse(left, right, count);
    }
    return EqualScalar(left, right, count);
}

void CopyDispatch(std::uint32_t* out, const std::uint32_t* in, std::size_t count, Path path) {
    if (path == Path::Avx2) {
        CopyAvx2(out, in, count);
        return;
    }
    if (path == Path::Sse) {
        CopySse(out, in, count);
        return;
    }
    CopyScalar(out, in, count);
}

} // namespace AgcDriver::CpuBackend
