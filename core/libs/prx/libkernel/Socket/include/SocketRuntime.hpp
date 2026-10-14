#pragma once
namespace GuestSockets {
// Guest socket descriptors are small integers like on the console, so games
// can use them in fd_set arithmetic. Each one reserves a real OS descriptor
// (held open for the socket's lifetime) so the OS allocator never reuses
// the number for a file or pipe.
int Close(int descriptor);
bool IsOpen(int descriptor);
// True when descriptor is an open socket; revents is the ready subset of
// events using KernelSocketPoll flag bits (0 when nothing is ready).
bool QuerySocket(int descriptor, short events, short& revents);
}
