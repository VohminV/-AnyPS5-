#include "prx/libScePad/include/InputMapping.hpp"

// Gamepad-only input: no keyboard/mouse bindings.
// anyps5-input.ini and ANYPS5_INPUT_CONFIG are intentionally ignored:
// the pad is driven exclusively by SDL_GameController (Xbox/PS layout).
// Keeping this TU ensures PadInput links without the old KEY/MOUSE/WHEEL parser.

std::vector<Pad::InputBinding> Pad::LoadInputMapping() {
    return {};
}
