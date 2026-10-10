#ifndef CORE_LIBS_PRX_LIBSCEPAD_INPUTMAPPING_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_INPUTMAPPING_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "PadInputTypes.hpp"

namespace Pad {

// Gamepad-only input: keyboard/mouse bindings were removed.
// The pad is driven exclusively by SDL_GameController (Xbox/PS layout,
// see PadInput::sampleController). No built-in KEY/MOUSE/WHEEL bindings.
inline constexpr std::array<InputBinding, 0> InputMapping{};

std::vector<InputBinding> LoadInputMapping();

}

#endif
