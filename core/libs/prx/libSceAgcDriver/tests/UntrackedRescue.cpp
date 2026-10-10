#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::Graphics::DecideUntrackedRescue;
using AgcDriver::Graphics::UntrackedRescueVerdict;
using AgcDriver::Graphics::UntrackedRescueVerdictName;

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error("UntrackedRescue: " + reason);
}

void RequireVerdict(bool keysChanged, bool cpuWrote, bool untrackedOnly, bool hasAlias, UntrackedRescueVerdict expected, const std::string& reason) {
    const auto verdict = DecideUntrackedRescue(keysChanged, cpuWrote, untrackedOnly, hasAlias);
    Require(verdict == expected, reason + " (got " + UntrackedRescueVerdictName(verdict) + ", want " + UntrackedRescueVerdictName(expected) + ")");
}

} // namespace

// Regression test for the untracked-range Refresh rescue (Texture.cpp): the
// rescue may fire only when every gate passes, for clean images and pending
// results alike. Gate order matters: key and CPU evidence forbid it before
// the layer/alias state is even considered, tracked layers keep the
// pre-existing drop/store path, and aliases keep the borrow path. Unknown
// state (tracked evidence, alias present) must never rescue.
void RunUntrackedRescueTests() {
    // The rescue case: changed evidence from dead stamps only.
    RequireVerdict(false, false, true, false, UntrackedRescueVerdict::Rescue, "untracked-only must rescue");
    // Key evidence wins over everything: a DCC flip is a real content change.
    RequireVerdict(true, false, true, false, UntrackedRescueVerdict::SkipKeysChanged, "keysChanged must forbid");
    RequireVerdict(true, true, true, true, UntrackedRescueVerdict::SkipKeysChanged, "keysChanged must win over cpu/alias");
    RequireVerdict(true, false, false, false, UntrackedRescueVerdict::SkipKeysChanged, "keysChanged must win over tracked");
    // CPU-stamped blocks are real writes: the CPU's bytes win.
    RequireVerdict(false, true, true, false, UntrackedRescueVerdict::SkipCpuWrote, "cpuWrote must forbid");
    RequireVerdict(false, true, false, true, UntrackedRescueVerdict::SkipCpuWrote, "cpuWrote must win over alias");
    // Tracked layers carry real stamp evidence: existing path owns it.
    RequireVerdict(false, false, false, false, UntrackedRescueVerdict::SkipTrackedPending, "tracked layers must keep the old path");
    // An alias borrows units on the device: the borrow path owns the case.
    RequireVerdict(false, false, true, true, UntrackedRescueVerdict::SkipAlias, "alias must keep the borrow path");
    // Every verdict has a distinct short name for the [untracked-rescue] line.
    Require(std::string(UntrackedRescueVerdictName(UntrackedRescueVerdict::Rescue)) == "rescue", "rescue name");
    Require(std::string(UntrackedRescueVerdictName(UntrackedRescueVerdict::SkipAlias)) == "alias", "alias name");
}
