#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "SDL.h"
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libScePad/include/PadInputTypes.hpp"
#include "prx/libc/include/General.hpp"

PadInput::PadInput()
    : bindings(Pad::LoadInputMapping()) {
    // Gamepad-only: bindings is always empty. Xbox/PS controllers are handled
    // purely through SDL_GameController in sampleController() below.
    openFirstAvailableController();
}

PadInput::~PadInput() {
    closeController();
}

void PadInput::openFirstAvailableController() {
    if (controller != nullptr) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            APS5_LOG_ERR("Pad: SDL game controller init failed: %s", SDL_GetError());
            return;
        }
    }
    for (int deviceIndex = 0; deviceIndex < SDL_NumJoysticks(); ++deviceIndex) {
        if (!SDL_IsGameController(deviceIndex)) continue;
        openController(deviceIndex);
        if (controller != nullptr) return;
    }
}

void PadInput::openController(int deviceIndex) {
    if (controller != nullptr || !SDL_IsGameController(deviceIndex)) return;
    controller = SDL_GameControllerOpen(deviceIndex);
    if (controller == nullptr) {
        APS5_LOG_ERR("Pad: could not open game controller %d: %s", deviceIndex, SDL_GetError());
        return;
    }
    const char* name = SDL_GameControllerName(controller);
    APS5_LOG_OUT("Pad: connected game controller: %s (type %d, sensors accel=%d gyro=%d, touchpads=%d, led=%d, trigger rumble=%d)",
        name != nullptr ? name : "unknown", static_cast<int>(SDL_GameControllerGetType(controller)),
        SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE, SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE,
        SDL_GameControllerGetNumTouchpads(controller), SDL_GameControllerHasLED(controller) == SDL_TRUE, SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE);
    enableSensors();
    outputPending = true;
}

void PadInput::enableSensors() {
    if (controller == nullptr) return;
    const SDL_bool wanted = outputState.motionEnabled ? SDL_TRUE : SDL_FALSE;
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, wanted);
    if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE) SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, wanted);
}

void PadInput::closeController() {
    if (controller == nullptr) return;
    SDL_GameControllerClose(controller);
    controller = nullptr;
    controllerState = {};
}

void PadInput::applyOutput() {
    // ANYPS5_RUMBLE=0 disables all force feedback (real toggle for the launcher).
    static const bool rumbleEnabled = [] {
        const char* raw = std::getenv("ANYPS5_RUMBLE");
        return raw == nullptr || std::string(raw) != "0";
    }();
    PadOutputState fetched;
    if (PadFetchOutput_nid_postfix(&outputSequence, &fetched)) {
        const bool motionChanged = fetched.motionEnabled != outputState.motionEnabled;
        outputState = fetched;
        outputPending = true;
        if (motionChanged) enableSensors();
    }
    if (controller == nullptr) return;
    const auto now = std::chrono::steady_clock::now();
    const bool rumbling = outputState.vibrationLarge != 0 || outputState.vibrationSmall != 0;
    const bool triggerRumble = outputState.trigger[0].fallback != 0 || outputState.trigger[1].fallback != 0;
    const bool isPs5 = SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5;
    if (!outputPending) {
        if ((rumbling || (triggerRumble && !isPs5)) && now >= nextRumbleRefresh) outputPending = true;
        else return;
    }
    outputPending = false;
    nextRumbleRefresh = now + std::chrono::milliseconds(700);
    constexpr Uint32 rumbleMs = 2000;
    if (!rumbleEnabled) {
        // Hard stop: never rumble when disabled, and stop any active effect.
        SDL_GameControllerRumble(controller, 0, 0, 0);
        return;
    }
    SDL_GameControllerRumble(controller, static_cast<Uint16>(outputState.vibrationLarge * 257), static_cast<Uint16>(outputState.vibrationSmall * 257), rumbling ? rumbleMs : 0);
    if (SDL_GameControllerHasLED(controller) == SDL_TRUE) {
        if (outputState.lightBarValid) SDL_GameControllerSetLED(controller, outputState.lightBar[0], outputState.lightBar[1], outputState.lightBar[2]);
        else SDL_GameControllerSetLED(controller, 0, 64, 255);
    }
    if (outputState.triggerTouched) {
        if (isPs5) {
            Uint8 effect[47] = {};
            effect[0] = 0x04 | 0x08;
            std::memcpy(effect + 10, outputState.trigger[1].effect, 11);
            std::memcpy(effect + 21, outputState.trigger[0].effect, 11);
            SDL_GameControllerSendEffect(controller, effect, sizeof(effect));
        } else if (SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE) {
            SDL_GameControllerRumbleTriggers(controller, static_cast<Uint16>(outputState.trigger[0].fallback * 257), static_cast<Uint16>(outputState.trigger[1].fallback * 257), triggerRumble ? rumbleMs : 0);
        }
    }
}

PadInputState PadInput::sampleController() const {
    PadInputState result;
    if (controller == nullptr) return result;
    const auto readButton = [this](SDL_GameControllerButton button) {
        return SDL_GameControllerGetButton(controller, button) != 0;
    };
    const auto addButton = [&result, &readButton](SDL_GameControllerButton source, Pad::PadButton button) {
        if (readButton(source)) result.buttons |= static_cast<std::uint32_t>(button);
    };
    // Standard Xbox <-> PlayStation layout (verified against SDL docs):
    // Xbox A (south) -> Cross (south), B (east) -> Circle (east),
    // X (west) -> Square (west), Y (north) -> Triangle (north).
    // LB/RB -> L1/R1, LT/RT axes -> analog L2/R2, Menu/Start -> Options,
    // View/Back -> TouchPad tap emulation (Xbox has no touchpad),
    // D-pad + stick clicks map 1:1.
    // ANYPS5_SWAP_AB=1 swaps A/B (Cross/Circle) for users with a swapped layout.
    static const bool swapAB = [] {
        const char* raw = std::getenv("ANYPS5_SWAP_AB");
        return raw != nullptr && std::string(raw) == "1";
    }();
    if (!swapAB) {
        addButton(SDL_CONTROLLER_BUTTON_A, Pad::PadButton::Cross);
        addButton(SDL_CONTROLLER_BUTTON_B, Pad::PadButton::Circle);
    } else {
        addButton(SDL_CONTROLLER_BUTTON_A, Pad::PadButton::Circle);
        addButton(SDL_CONTROLLER_BUTTON_B, Pad::PadButton::Cross);
    }
    addButton(SDL_CONTROLLER_BUTTON_X, Pad::PadButton::Square);
    addButton(SDL_CONTROLLER_BUTTON_Y, Pad::PadButton::Triangle);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Pad::PadButton::L1);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, Pad::PadButton::R1);
    const auto type = SDL_GameControllerGetType(controller);
    const bool viewPressed = type != SDL_CONTROLLER_TYPE_PS4 && type != SDL_CONTROLLER_TYPE_PS5 && readButton(SDL_CONTROLLER_BUTTON_BACK);
    if (viewPressed) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::TouchPad);
    addButton(SDL_CONTROLLER_BUTTON_START, Pad::PadButton::Options);
    addButton(SDL_CONTROLLER_BUTTON_LEFTSTICK, Pad::PadButton::L3);
    addButton(SDL_CONTROLLER_BUTTON_RIGHTSTICK, Pad::PadButton::R3);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_UP, Pad::PadButton::Up);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, Pad::PadButton::Right);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, Pad::PadButton::Down);
    addButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, Pad::PadButton::Left);
    addButton(SDL_CONTROLLER_BUTTON_TOUCHPAD, Pad::PadButton::TouchPad);

    const auto triggerValue = [this](SDL_GameControllerAxis axis) {
        const auto value = std::clamp<int>(SDL_GameControllerGetAxis(controller, axis), 0, 32767);
        return static_cast<std::uint8_t>((value * 255 + 16383) / 32767);
    };
    result.analogButtonsL2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    result.analogButtonsR2 = triggerValue(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (result.analogButtonsL2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::L2);
    if (result.analogButtonsR2 != 0) result.buttons |= static_cast<std::uint32_t>(Pad::PadButton::R2);

    const auto stickValue = [this](SDL_GameControllerAxis axis) {
        const auto value = static_cast<std::int32_t>(SDL_GameControllerGetAxis(controller, axis)) + 32768;
        return static_cast<std::uint8_t>((value * 255 + 32767) / 65535);
    };
    // ANYPS5_STICK_DEADZONE: radial deadzone in 0..255 units around center 128
    // (default 10). Prevents drift on worn Chinese sticks without killing response.
    static const int deadzone = [] {
        const char* raw = std::getenv("ANYPS5_STICK_DEADZONE");
        if (raw == nullptr || *raw == '\0') return 10;
        char* end = nullptr;
        const long v = std::strtol(raw, &end, 10);
        if (end == raw || v < 0 || v > 64) return 10;
        return static_cast<int>(v);
    }();
    auto axis = stickValue(SDL_CONTROLLER_AXIS_LEFTX);
    auto axisY = stickValue(SDL_CONTROLLER_AXIS_LEFTY);
    auto axisRX = stickValue(SDL_CONTROLLER_AXIS_RIGHTX);
    auto axisRY = stickValue(SDL_CONTROLLER_AXIS_RIGHTY);
    const auto applyDz = [](std::uint8_t v) {
        const int d = static_cast<int>(v) - 128;
        return (d < 0 ? -d : d) <= deadzone ? std::uint8_t{128} : v;
    };
    result.sticks = {applyDz(axis), applyDz(axisY), applyDz(axisRX), applyDz(axisRY)};
    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_PS5: result.deviceKind = 1; break;
        case SDL_CONTROLLER_TYPE_PS4: result.deviceKind = 2; break;
        default: result.deviceKind = 3; break;
    }
    if (SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_ACCEL) == SDL_TRUE && SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_GYRO) == SDL_TRUE) {
        float accel[3];
        float gyro[3];
        if (SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL, accel, 3) == 0 && SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, gyro, 3) == 0) {
            result.hasMotion = true;
            for (int i = 0; i < 3; ++i) { result.accel[i] = accel[i]; result.gyro[i] = gyro[i]; }
        }
    }
    if (SDL_GameControllerGetNumTouchpads(controller) > 0) {
        for (int finger = 0; finger < 2; ++finger) {
            Uint8 down = 0;
            float x = 0.0f;
            float y = 0.0f;
            float pressure = 0.0f;
            if (SDL_GameControllerGetTouchpadFinger(controller, 0, finger, &down, &x, &y, &pressure) != 0 || down == 0) continue;
            result.touch[finger].active = true;
            result.touch[finger].x = static_cast<std::uint16_t>(std::clamp(x, 0.0f, 1.0f) * 1919.0f);
            result.touch[finger].y = static_cast<std::uint16_t>(std::clamp(y, 0.0f, 1.0f) * 942.0f);
        }
    }
    if (viewPressed && !result.touch[0].active && !result.touch[1].active) {
        result.touch[0] = {true, 960, 471, 0};
    }
    return result;
}

void PadInput::HandleEvent(const SDL_Event& event, DisplayWindow& window) {
    static_cast<void>(window);
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        openController(event.cdevice.which);
        return;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller != nullptr) {
        const auto instanceId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        if (instanceId == event.cdevice.which) {
            closeController();
            openFirstAvailableController();
            publish();
        }
        return;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP ||
        event.type == SDL_CONTROLLERAXISMOTION || event.type == SDL_CONTROLLERDEVICEREMAPPED) {
        publish();
        return;
    }
    // Gamepad-only: keyboard/mouse/wheel events are intentionally ignored.
    // No ToggleHelp (F1), no ToggleFullscreen (F11), no ToggleMouse.
}

void PadInput::Update() {
    if (controller != nullptr) SDL_GameControllerUpdate();
    applyOutput();
    if (controller != nullptr) {
        controllerState = sampleController();
        publish();
    }
}

void PadInput::publish() {
    // Gamepad-only: publish the sampled controller state directly.
    PadPublishInput_nid_postfix(controllerState);
}
