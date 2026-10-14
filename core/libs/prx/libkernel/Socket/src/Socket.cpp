#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/Socket/include/SocketPoll.hpp"
#include "prx/libkernel/Socket/include/SocketRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <errno.h>
#include <poll.h>
#endif

namespace {

constexpr int GuestEinval = 22;
constexpr int SelectDescriptorLimit = 1024;

struct GuestTimeval {
    std::int64_t seconds;
    std::int64_t microseconds;
};

std::atomic<KernelSocketPoll::Poller> g_socketPoller{nullptr};

bool DescriptorSet(const void* set, int descriptor) {
    return set != nullptr && ((static_cast<const std::uint64_t*>(set)[descriptor / 64] >> (descriptor % 64)) & 1u) != 0;
}

void ClearDescriptor(void* set, int descriptor) {
    if (set != nullptr) static_cast<std::uint64_t*>(set)[descriptor / 64] &= ~(std::uint64_t{1} << (descriptor % 64));
}

enum class DirectProbe { Ready, BadDescriptor, Unknown };

// Poll a descriptor the socket poller does not own (pipes, regular files).
// Ready fills revents; BadDescriptor means POSIX EBADF; Unknown keeps the
// previous "not a socket" verdict for genuinely unpollable handles.
DirectProbe ProbeNonSocket(int descriptor, short events, short& revents) {
    revents = 0;
#ifdef _WIN32
    const intptr_t handle = _get_osfhandle(descriptor);
    if (handle == -1) return DirectProbe::BadDescriptor;
    const DWORD type = GetFileType(reinterpret_cast<HANDLE>(handle));
    if (type == FILE_TYPE_PIPE) {
        DWORD available = 0;
        if (PeekNamedPipe(reinterpret_cast<HANDLE>(handle), nullptr, 0, nullptr, &available, nullptr)) {
            if (available > 0 && (events & KernelSocketPoll::Readable) != 0) revents |= KernelSocketPoll::Readable;
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            if ((events & KernelSocketPoll::Readable) != 0) revents |= KernelSocketPoll::Readable | KernelSocketPoll::HangUp;
        } else if (GetLastError() != ERROR_ACCESS_DENIED) {
            return DirectProbe::Unknown;
        }
        // The write end rejects the peek above; pipes are reported writable
        // unless known otherwise, which matches how wakeup pipes are polled
        // (readability is what callers wait for).
        if ((events & KernelSocketPoll::Writable) != 0) revents |= KernelSocketPoll::Writable;
        return DirectProbe::Ready;
    }
    if (type == FILE_TYPE_DISK) {
        revents = events & (KernelSocketPoll::Readable | KernelSocketPoll::Writable | KernelSocketPoll::Urgent);
        return DirectProbe::Ready;
    }
    if (type == FILE_TYPE_CHAR) {
        // Consoles and NUL: readability is real input availability, writes
        // to an output-like character device always succeed.
        DWORD pending = 0;
        if (GetNumberOfConsoleInputEvents(reinterpret_cast<HANDLE>(handle), &pending)) {
            if (pending > 0 && (events & KernelSocketPoll::Readable) != 0) revents |= KernelSocketPoll::Readable;
        }
        if ((events & KernelSocketPoll::Writable) != 0) revents |= KernelSocketPoll::Writable;
        return DirectProbe::Ready;
    }
    return DirectProbe::Unknown;
#else
    struct pollfd watched{descriptor, POLLIN | POLLPRI | POLLOUT, 0};
    const int result = ::poll(&watched, 1, 0);
    if (result < 0) return errno == EBADF ? DirectProbe::BadDescriptor : DirectProbe::Unknown;
    if (result == 0) return DirectProbe::Ready;
    if ((watched.revents & (POLLIN | POLLHUP | POLLERR)) != 0 && (events & KernelSocketPoll::Readable) != 0)
        revents |= KernelSocketPoll::Readable;
    if ((watched.revents & POLLHUP) != 0) revents |= KernelSocketPoll::HangUp;
    if ((watched.revents & POLLERR) != 0) revents |= KernelSocketPoll::Error;
    if ((watched.revents & POLLOUT) != 0 && (events & KernelSocketPoll::Writable) != 0)
        revents |= KernelSocketPoll::Writable;
    if ((watched.revents & POLLPRI) != 0 && (events & KernelSocketPoll::Urgent) != 0)
        revents |= KernelSocketPoll::Urgent;
    return DirectProbe::Ready;
#endif
}

}

extern "C" {

const char* APS5_VABI __inet_ntop_nid_postfix(int family, const void* source, char* destination, std::uint32_t capacity);
int APS5_VABI __inet_pton_nid_postfix(int family, const char* text, void* destination);

const char* APS5_VABI inet_ntop_nid_postfix(int af, const void* src, char* dst, uint32_t size) {
    return __inet_ntop_nid_postfix(af, src, dst, size);
}

int APS5_VABI inet_pton_nid_postfix(int af, const char* src, void* dst) {
    return __inet_pton_nid_postfix(af, src, dst);
}

int* APS5_VABI __error_nid_postfix();

void KernelSetSocketPoller_nid_no_patch(KernelSocketPoll::Poller poller) {
    g_socketPoller.store(poller);
}

int APS5_VABI select_nid_postfix(int nfds, void* readfds, void* writefds, void* exceptfds, const void* timeout) {
    const auto* limit = static_cast<const GuestTimeval*>(timeout);
    if (nfds < 0 || nfds > SelectDescriptorLimit || (limit != nullptr && (limit->seconds < 0 || limit->microseconds < 0 || limit->microseconds >= 1000000))) {
        *__error_nid_postfix() = GuestEinval;
        return -1;
    }
    std::vector<KernelSocketPoll::Entry> entries;
    for (int descriptor = 0; descriptor < nfds; ++descriptor) {
        short events = 0;
        if (DescriptorSet(readfds, descriptor)) events |= KernelSocketPoll::Readable;
        if (DescriptorSet(writefds, descriptor)) events |= KernelSocketPoll::Writable;
        if (DescriptorSet(exceptfds, descriptor)) events |= KernelSocketPoll::Urgent;
        if (events != 0) entries.push_back({descriptor, events, 0});
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = limit == nullptr ? std::chrono::steady_clock::time_point::max()
        : start + std::chrono::seconds(limit->seconds) + std::chrono::microseconds(limit->microseconds);
    for (;;) {
        const auto passStart = std::chrono::steady_clock::now();
        int waitMilliseconds = 100;
        if (limit != nullptr) {
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now()).count();
            waitMilliseconds = static_cast<int>(std::clamp<std::int64_t>((remaining + 999) / 1000, 0, 100));
        }
        if (!entries.empty()) {
            const auto poller = g_socketPoller.load();
            if (poller != nullptr) {
                for (auto& entry : entries) entry.revents = 0;
                const int result = poller(entries.data(), static_cast<int>(entries.size()), waitMilliseconds);
                if (result < 0) {
                    *__error_nid_postfix() = -result;
                    return -1;
                }
            } else {
                for (auto& entry : entries) entry.revents = KernelSocketPoll::Unknown;
            }
            bool badDescriptor = false;
            for (auto& entry : entries) {
                if ((entry.revents & KernelSocketPoll::Unknown) == 0) continue;
                // libkernel sockets live in their own table (invisible to the
                // libSceNet poller); poll the real OS socket, not the reserved
                // placeholder, so incoming data actually wakes select().
                short queried = 0;
                if (GuestSockets::QuerySocket(entry.descriptor, entry.events, queried)) {
                    entry.revents = queried;
                    continue;
                }
                short probed = 0;
                switch (ProbeNonSocket(entry.descriptor, entry.events, probed)) {
                case DirectProbe::Ready: entry.revents = probed; break;
                case DirectProbe::BadDescriptor: badDescriptor = true; break;
                case DirectProbe::Unknown: break;
                }
            }
            if (badDescriptor) {
                *__error_nid_postfix() = 9;
                return -1;
            }
            int ready = 0;
            for (const auto& entry : entries) {
                if ((entry.revents & KernelSocketPoll::Unknown) != 0) {
                    const std::string what = "select_nid_postfix: descriptor " + std::to_string(entry.descriptor) +
                                             " is not a libSceNet socket, pipe, or file";
                    NotImplemented_nid_no_patch(what.c_str());
                }
                const bool readable = (entry.events & KernelSocketPoll::Readable) != 0 && (entry.revents & (KernelSocketPoll::Readable | KernelSocketPoll::HangUp | KernelSocketPoll::Error)) != 0;
                const bool writable = (entry.events & KernelSocketPoll::Writable) != 0 && (entry.revents & (KernelSocketPoll::Writable | KernelSocketPoll::Error)) != 0;
                const bool urgent = (entry.events & KernelSocketPoll::Urgent) != 0 && (entry.revents & KernelSocketPoll::Urgent) != 0;
                ready += static_cast<int>(readable) + static_cast<int>(writable) + static_cast<int>(urgent);
            }
            if (ready > 0) {
                for (const auto& entry : entries) {
                    if ((entry.revents & (KernelSocketPoll::Readable | KernelSocketPoll::HangUp | KernelSocketPoll::Error)) == 0) ClearDescriptor(readfds, entry.descriptor);
                    if ((entry.revents & (KernelSocketPoll::Writable | KernelSocketPoll::Error)) == 0) ClearDescriptor(writefds, entry.descriptor);
                    if ((entry.revents & KernelSocketPoll::Urgent) == 0) ClearDescriptor(exceptfds, entry.descriptor);
                }
                return ready;
            }
            // Nothing is ready. Pace idle passes: neither the poller (absent
            // or with no descriptors of its own) nor the zero-timeout probes
            // above block, so without this the loop busy-spins. Skip the nap
            // when this pass already waited (a blocking poller call).
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - passStart).count();
            if (waitMilliseconds > 0 && elapsed < waitMilliseconds / 2) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(waitMilliseconds - static_cast<int>(elapsed)));
            }
        } else if (waitMilliseconds > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMilliseconds));
        }
        if (limit != nullptr && std::chrono::steady_clock::now() >= deadline) {
            for (const auto& entry : entries) {
                ClearDescriptor(readfds, entry.descriptor);
                ClearDescriptor(writefds, entry.descriptor);
                ClearDescriptor(exceptfds, entry.descriptor);
            }
            return 0;
        }
    }
}

}
