#pragma once
// Input manager: XInput (primary for Xbox-compatible pads incl. Chinese
// USB-dongle clones) + WinMM joystick names/diagnostics for DirectInput/HID.
// No assumptions: reports the actually supported interface.
#include <windows.h>
#include <string>
#include <vector>

namespace launcher {

struct PadState {
    bool connected = false;
    std::string api; // "XInput" | "WinMM" | ""
    int slot = -1;
    // buttons
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false, menu = false, view = false;
    bool l3 = false, r3 = false;
    bool dup = false, ddown = false, dleft = false, dright = false;
    int lx = 0, ly = 0, rx = 0, ry = 0; // -32768..32767
    int lt = 0, rt = 0;                 // 0..255
};

struct PadDevice {
    std::string name;
    std::string api;
    int slot = -1;
    bool connected = false;
};

class InputManager {
public:
    InputManager();
    ~InputManager();
    void Poll(); // refresh states
    std::vector<PadDevice> Devices(); // XInput slots + WinMM sticks
    bool AnyConnected();
    const PadState& Primary() const { return primary_; }
    // Navigation edge events (debounced): dpad/stick + A/B/LB/RB/Menu/View.
    struct NavEvent {
        enum Type { None, Up, Down, Left, Right, Confirm, Back, TabLeft, TabRight, Menu, View } type = None;
    };
    NavEvent ConsumeNav(); // call after Poll()
    bool RumbleTest(int ms = 800); // XInputSetState both motors
    static const char* SchemeText(bool swapAB);

private:
    bool pollXInput(int slot, PadState& out);
    bool pollWinMM(int id, PadState& out);

    HMODULE xinput_ = nullptr;
    using XIGetState = DWORD(WINAPI*)(DWORD, void*);
    using XISetState = DWORD(WINAPI*)(DWORD, void*);
    using XIGetBattery = DWORD(WINAPI*)(DWORD, BYTE, void*);
    XIGetState pGetState_ = nullptr;
    XISetState pSetState_ = nullptr;

    PadState slots_[4];
    PadState primary_;
    DWORD prevButtons_ = 0;
    DWORD lastNavMs_ = 0;
    int lastStickDir_ = 0;
    DWORD lastRepeatMs_ = 0;
};

} // namespace launcher
