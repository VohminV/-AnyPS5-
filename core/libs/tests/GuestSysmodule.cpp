#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>

extern "C" {
int APS5_VABI sceSysmoduleLoadModuleInternal(std::uint32_t id);
int APS5_VABI sceSysmoduleUnloadModuleInternal(std::uint32_t id);
int APS5_VABI sceSysmoduleLoadModule(std::uint16_t id);
int APS5_VABI sceSysmoduleIsLoaded(std::uint16_t id);
int APS5_VABI sceSysmoduleUnloadModule(std::uint16_t id);
}

namespace {

void Require(bool value) { if (!value) std::abort(); }

constexpr std::uint32_t kFiberModuleId = 0x00000006;
constexpr int kModuleNotLoaded = static_cast<int>(0x80A90003);
// Ghost of Yotei asks for PS5-new IDs past the PS4 0x107 range. They have no
// public name and no local lib; the non-internal loader must keep its
// historical safe-stub (success / not-loaded) so boot continues. No name is
// invented here.
constexpr std::uint16_t kGhostModule271 = 271;
constexpr std::uint16_t kGhostModule272 = 272;

}

int main() {
    Require(sceSysmoduleLoadModuleInternal(kFiberModuleId) == 0);
    Require(sceSysmoduleLoadModuleInternal(kFiberModuleId) == 0);
    Require(sceSysmoduleUnloadModuleInternal(kFiberModuleId) == 0);
    Require(sceSysmoduleUnloadModuleInternal(kFiberModuleId) == 0);
    Require(sceSysmoduleUnloadModuleInternal(kFiberModuleId) == kModuleNotLoaded);
    Require(sceSysmoduleLoadModule(kGhostModule271) == 0);
    Require(sceSysmoduleLoadModule(kGhostModule272) == 0);
    Require(sceSysmoduleIsLoaded(kGhostModule271) == 0);
    Require(sceSysmoduleUnloadModule(kGhostModule271) == 0);
    Require(sceSysmoduleUnloadModule(kGhostModule272) == 0);
}
