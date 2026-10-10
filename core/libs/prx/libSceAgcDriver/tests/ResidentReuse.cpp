#include "GraphicsTests.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include <stdexcept>
#include <string>

namespace {

using AgcDriver::Graphics::DecideResidentReuse;
using AgcDriver::Graphics::ResidentReuseVerdict;
using AgcDriver::Graphics::ResidentReuseVerdictName;

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error("ResidentReuse: " + reason);
}

void RequireVerdict(bool snapshotValid, bool ownerCurrent, bool hasBorrowed, bool keysProved, bool keysEqual, bool hasForeignPending, bool remapped, ResidentReuseVerdict expected, const std::string& reason) {
    const auto verdict = DecideResidentReuse(snapshotValid, ownerCurrent, hasBorrowed, keysProved, keysEqual, hasForeignPending, remapped);
    Require(verdict == expected, reason + " (got " + ResidentReuseVerdictName(verdict) + ", want " + ResidentReuseVerdictName(expected) + ")");
}

} // namespace

// Regression test for the resident fast-path of untracked surfaces
// (StorageTexture::Refresh): an unchanged resident image must be provable
// without a full refresh, while every unknown or changed state keeps the
// slow path. Gate order matters: without ground truth (snapshot) nothing is
// attempted; a changed owner/backing, borrowed alias units, unproved or
// differing keys, foreign pending results and remapped memory each forbid the
// proof before any byte-compare runs. MarkDirty() alone never proves
// anything here: it only moves the pending serial, which the memo check (not
// this pure gate) turns into a re-compare.
void RunResidentReuseTests() {
    // The reuse case: snapshot present, owner current, no borrow, keys proved
    // and equal, no foreign pending, mapping unchanged -> attempt the proof
    // (memo hit else byte-compare).
    RequireVerdict(true, true, false, true, true, false, false, ResidentReuseVerdict::AttemptProof, "clean resident must attempt proof");
    // No ground truth: the slow path owns it (it takes the snapshot on upload).
    RequireVerdict(false, true, false, true, true, false, false, ResidentReuseVerdict::SkipNoSnapshot, "missing snapshot must keep slow path");
    // Backing resource changed: pending GPU results are stale for the new owner.
    RequireVerdict(true, false, false, true, true, false, false, ResidentReuseVerdict::SkipOwnerChanged, "owner change must forbid");
    RequireVerdict(true, false, true, true, true, true, true, ResidentReuseVerdict::SkipOwnerChanged, "owner change must win over borrow/pending/remap");
    // Alias borrow: the borrow path owns the case.
    RequireVerdict(true, true, true, true, true, false, false, ResidentReuseVerdict::SkipBorrowed, "borrowed units must keep borrow path");
    // Keys: unproved or differing keys are real content changes.
    RequireVerdict(true, true, false, false, true, false, false, ResidentReuseVerdict::SkipKeysUnproved, "unproved keys must forbid");
    RequireVerdict(true, true, false, false, false, true, true, ResidentReuseVerdict::SkipKeysUnproved, "unproved keys must win over differ/pending/remap");
    RequireVerdict(true, true, false, true, false, false, false, ResidentReuseVerdict::SkipKeysDiffer, "differing keys must forbid");
    // Foreign pending results: the slow path flushes them first.
    RequireVerdict(true, true, false, true, true, true, false, ResidentReuseVerdict::SkipForeignPending, "foreign pending must forbid");
    // Remapped memory: the snapshot is ground truth only under its mapping.
    RequireVerdict(true, true, false, true, true, false, true, ResidentReuseVerdict::SkipRemapped, "remap must forbid");
    // Every verdict has a distinct short name for logs.
    Require(std::string(ResidentReuseVerdictName(ResidentReuseVerdict::AttemptProof)) == "attempt", "attempt name");
    Require(std::string(ResidentReuseVerdictName(ResidentReuseVerdict::SkipForeignPending)) == "foreign-pending", "foreign-pending name");
    Require(std::string(ResidentReuseVerdictName(ResidentReuseVerdict::SkipRemapped)) == "remapped", "remapped name");
}
