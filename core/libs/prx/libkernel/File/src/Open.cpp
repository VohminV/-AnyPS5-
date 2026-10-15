#include "prx/libkernel/File/include/FileFlags.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libc/include/CrashNotes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libkernel/File/include/File.hpp"
#include "prx/libkernel/File/include/DirectoryDescriptor.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"
#include "SceTypes.hpp"

#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::_wopen(p.wstring().c_str(), nativeFlags, static_cast<int>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::_lseeki64(fd, offset, whence);
}
static int NativeRead(int fd, void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        errno = EINVAL;
        return -1;
    }
    return ::_read(fd, buf, static_cast<unsigned int>(n));
}
static int NativeWrite(int fd, const void* buf, std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
        errno = EINVAL;
        return -1;
    }
    return ::_write(fd, buf, static_cast<unsigned int>(n));
}
extern "C" _invalid_parameter_handler _set_thread_local_invalid_parameter_handler(_invalid_parameter_handler);
static void IgnoreInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}
static int NativeClose(int fd) {
    const auto previous = _set_thread_local_invalid_parameter_handler(IgnoreInvalidParameter);
    const int result = ::_close(fd);
    _set_thread_local_invalid_parameter_handler(previous);
    return result;
}
static int NativeUnlink(const std::filesystem::path& p) {
    return ::_wunlink(p.wstring().c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= _O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= _O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= _O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= _O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= _O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= _O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= _O_EXCL;
    f |= _O_BINARY;
    return f;
}
#else
#include <fcntl.h>
#include <unistd.h>
static int NativeOpen(const std::filesystem::path& p, int nativeFlags, std::uint16_t mode) {
    return ::open(p.c_str(), nativeFlags, static_cast<mode_t>(mode));
}
static std::int64_t NativeLseek(int fd, std::int64_t offset, int whence) {
    return ::lseek(fd, static_cast<off_t>(offset), whence);
}
static std::int64_t NativeRead(int fd, void* buf, std::size_t n) {
    return ::read(fd, buf, n);
}
static std::int64_t NativeWrite(int fd, const void* buf, std::size_t n) {
    return ::write(fd, buf, n);
}
static int NativeClose(int fd) { return ::close(fd); }
static int NativeUnlink(const std::filesystem::path& p) {
    return ::unlink(p.c_str());
}
static int MapFlags(int sceFlags) {
    int f = 0;
    const int acc = sceFlags & SCE_KERNEL_O_ACCMODE;
    if (acc == SCE_KERNEL_O_RDONLY) f |= O_RDONLY;
    else if (acc == SCE_KERNEL_O_WRONLY) f |= O_WRONLY;
    else if (acc == SCE_KERNEL_O_RDWR) f |= O_RDWR;
    else throw std::invalid_argument("sceKernelOpen: invalid access mode");
    if (sceFlags & SCE_KERNEL_O_APPEND) f |= O_APPEND;
    if (sceFlags & SCE_KERNEL_O_CREAT) f |= O_CREAT;
    if (sceFlags & SCE_KERNEL_O_TRUNC) f |= O_TRUNC;
    if (sceFlags & SCE_KERNEL_O_EXCL) f |= O_EXCL;
    if (sceFlags & SCE_KERNEL_O_SYNC) f |= O_SYNC;
    if (sceFlags & SCE_KERNEL_O_DIRECTORY) f |= O_DIRECTORY;
    return f;
}
#endif

static int SceErrorFromErrno(int error) {
    constexpr int GuestEio = 5;
    const int guest = error > 0 && error <= 34 ? error : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(guest));
}

// Socket I/O lives in Socket/src/Udp.cpp. Like the console (one fd table
// for files and sockets), sceKernelRead/Write on a socket descriptor sends
// and receives; failing it with EBADF stalls online code (EOS signaling).
static int SceErrorFromGuestErrno(int guest) {
    // Socket failures already carry Orbis errno numbers (EAGAIN=35,
    // ENOTCONN=57, ...); keep the full range instead of folding to EIO.
    constexpr int GuestEio = 5;
    const int mapped = guest >= 1 && guest <= 94 ? guest : GuestEio;
    return static_cast<int>(0x80020000u | static_cast<unsigned>(mapped));
}
extern "C" {
std::int64_t APS5_VABI send_nid_postfix(int descriptor, const void* buffer, std::uint64_t length, int flags);
std::int64_t APS5_VABI recv_nid_postfix(int descriptor, void* buffer, std::uint64_t length, int flags);
int* APS5_VABI __error_nid_postfix();
}

namespace {

// The sandbox maps every absolute guest path under the process root, so
// /dev/urandom and /dev/random would never resolve to a host device.
// Materialize them as temp files filled from the OS CSPRNG
// (std::random_device); reads/lseek/close then behave like regular files
// and the backing file is deleted on close.
constexpr std::size_t RandomMaterializeSize = 4u * 1024u * 1024u;

bool IsRandomDevicePath(const char* path) {
    if (path == nullptr) return false;
    std::string text(path);
    for (auto& character : text) if (character == '\\') character = '/';
    return text == "/dev/urandom" || text == "/dev/random";
}

std::mutex randomFilesMutex;
std::map<int, std::filesystem::path> randomFiles;

std::filesystem::path WriteRandomMaterialization() {
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (error) throw std::runtime_error("sceKernelOpen: no temp directory for /dev/urandom");
    std::random_device entropy;
    std::vector<unsigned int> chunk(65536 / sizeof(unsigned int));
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = directory / ("anyps5-urandom-" + std::to_string(attempt) + ".bin");
        int out = -1;
#ifdef _WIN32
        _wsopen_s(&out, candidate.wstring().c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _SH_DENYRW,
                  static_cast<int>(0600));
#else
        out = ::open(candidate.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
        if (out < 0) continue;
        bool failed = false;
        for (std::size_t done = 0; done < RandomMaterializeSize && !failed; done += chunk.size() * sizeof(unsigned int)) {
            for (auto& word : chunk) word = entropy();
            const auto* bytes = reinterpret_cast<const char*>(chunk.data());
            std::size_t left = chunk.size() * sizeof(unsigned int);
            while (left != 0) {
#ifdef _WIN32
                const int wrote = ::_write(out, bytes, static_cast<unsigned int>(left));
#else
                const auto wrote = ::write(out, bytes, left);
#endif
                if (wrote <= 0) { failed = true; break; }
                bytes += wrote;
                left -= static_cast<std::size_t>(wrote);
            }
        }
#ifdef _WIN32
        ::_close(out);
#else
        ::close(out);
#endif
        if (failed) {
            std::filesystem::remove(candidate, error);
            continue;
        }
        return candidate;
    }
    throw std::runtime_error("sceKernelOpen: cannot materialize /dev/urandom");
}

void ForgetRandomFile(int fd) {
    std::filesystem::path backing;
    {
        std::lock_guard lock(randomFilesMutex);
        const auto found = randomFiles.find(fd);
        if (found == randomFiles.end()) return;
        backing = found->second;
        randomFiles.erase(found);
    }
    std::error_code error;
    std::filesystem::remove(backing, error);
}

}

extern "C" {

int APS5_VABI sceKernelOpen(const char* path, int flags, std::uint16_t mode) {
    APS5_LOG_OUT("path=%s flags=0x%X nativeFlags=0x%X mode=0%o", path, flags, MapFlags(flags), mode);
    if (path != nullptr && *path == '\0') return SceErrorFromErrno(2);
    if (IsRandomDevicePath(path)) {
        const int mapped = MapFlags(flags);
        const auto backing = WriteRandomMaterialization();
        int fd = NativeOpen(backing, mapped, mode);
        if (fd < 0) {
            std::error_code error;
            std::filesystem::remove(backing, error);
            CrashNotef_nid_no_patch("open", "%s -> %d", path, fd);
            return SceErrorFromErrno(errno);
        }
        {
            std::lock_guard lock(randomFilesMutex);
            randomFiles.emplace(fd, backing);
        }
        CrashNotef_nid_no_patch("open", "%s -> %d", path, fd);
        return fd;
    }
    auto native = ResolvePath_nid_no_patch(path);
    int fd = NativeOpen(native, MapFlags(flags), mode);
#ifdef _WIN32
    if (fd < 0) {
        std::error_code error;
        if (std::filesystem::is_directory(native, error)) fd = File::OpenDirectoryDescriptor(native);
    }
#endif
    CrashNotef_nid_no_patch("open", "%s -> %d", path != nullptr ? path : "?", fd);
    if (fd < 0) {
        return SceErrorFromErrno(errno);
    }
    if ((flags & SCE_KERNEL_O_ACCMODE) != SCE_KERNEL_O_RDONLY || (flags & (SCE_KERNEL_O_CREAT | SCE_KERNEL_O_TRUNC)))
        RecordWrittenPath_nid_no_patch(native);
    return fd;
}

int APS5_VABI sceKernelClose(int d) {
#ifdef _WIN32
    File::ForgetDirectoryDescriptor(d);
#endif
    if (GuestSockets::IsOpen(d)) {
        if (GuestSockets::Close(d) != 0) return SCE_KERNEL_ERROR_EBADF;
        return 0;
    }
    if (NativeClose(d) != 0) {
        if (errno == EBADF) return SCE_KERNEL_ERROR_EBADF;
        return SceErrorFromErrno(errno);
    }
    ForgetRandomFile(d);
    return 0;
}

std::int64_t APS5_VABI sceKernelRead(int d, void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) {
        return SCE_KERNEL_ERROR_EFAULT;
    }
    const GuestArena::HostWrite destination(buf, nbytes);
    if (!destination.Open()) return SCE_KERNEL_ERROR_EFAULT;
    if (GuestSockets::IsOpen(d)) {
        const auto n = recv_nid_postfix(d, buf, static_cast<std::uint64_t>(nbytes), 0);
        if (n < 0) {
            return SceErrorFromGuestErrno(*__error_nid_postfix());
        }
        return n;
    }
    auto n = NativeRead(d, buf, nbytes);
    if (n < 0) {
        return SceErrorFromErrno(errno);
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelWrite(int d, const void* buf, std::size_t nbytes) {
    if (buf == nullptr && nbytes != 0) {
        return SCE_KERNEL_ERROR_EFAULT;
    }
    if (GuestSockets::IsOpen(d)) {
        const auto n = send_nid_postfix(d, buf, static_cast<std::uint64_t>(nbytes), 0);
        if (n < 0) {
            return SceErrorFromGuestErrno(*__error_nid_postfix());
        }
        return n;
    }
    auto n = NativeWrite(d, buf, nbytes);
    if (n < 0) {
        return SceErrorFromErrno(errno);
    }
    return static_cast<std::int64_t>(n);
}

std::int64_t APS5_VABI sceKernelLseek(int d, std::int64_t offset, int whence) {
    if (whence < 0 || whence > 2) {
        return SCE_KERNEL_ERROR_EINVAL;
    }
    if (GuestSockets::IsOpen(d)) {
        return SCE_KERNEL_ERROR_ESPIPE;
    }
    std::int64_t result = NativeLseek(d, offset, whence);
    if (result < 0) {
        return SceErrorFromErrno(errno);
    }
    return result;
}

int APS5_VABI sceKernelStat(const char* path, FileStat* sb) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    if (sb == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": sb is null");
    }
    const auto native = ResolvePath_nid_no_patch(path);
    std::error_code error;
    if (!std::filesystem::exists(native, error)) {
        return SceErrorFromErrno(2);
    }
    File::FillFileStat(native, sb);
    return 0;
}

int APS5_VABI sceKernelUnlink(const char* path) {
    if (path == nullptr) {
        throw std::invalid_argument(std::string(__func__) + ": path is null");
    }
    auto native = ResolvePath_nid_no_patch(path);
    if (NativeUnlink(native) != 0) {
        return SceErrorFromErrno(errno);
    }
    RecordWrittenPath_nid_no_patch(native);
    return 0;
}

int APS5_VABI sceKernelFcntl() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
