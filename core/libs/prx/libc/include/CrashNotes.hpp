#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CRASHNOTES_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CRASHNOTES_HPP

// Flight recorder for post-mortem crash triage (see CrashReport.cpp).
//
// Any module may leave short notes about what it just did (an upload, a
// write-back, a present, a file open). The notes live in a fixed ring with
// no heap and no locks, so leaving one is safe on hot paths, and the crash
// handler dumps the last ones into crash.log beside the executable. This is
// what names the activity right before a death that happens elsewhere (for
// example a game-side allocator crash after a cutscene, while the driver was
// storing images or the title was opening bundles).
//
// Async-safety: leaving a note uses one atomic increment and bounded copies
// only. Dumping runs on the faulting thread and reads with bounded lengths,
// so a torn entry can garble text but can never crash the reporter.
// Set APS5_NO_CRASH_NOTES=1 to leave no notes.
extern "C" void CrashNote_nid_no_patch(const char* tag, const char* text);
extern "C" void CrashNotef_nid_no_patch(const char* tag, const char* format, ...);

#endif
