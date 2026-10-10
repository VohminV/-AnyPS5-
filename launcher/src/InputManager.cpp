#include "InputManager.hpp"
#include <mmsystem.h>
#include <XInput.h>
#include <cstdio>

namespace launcher {

InputManager::InputManager() {
    // Prefer xinput1_4, fall back to 9_1_0 / 1_3 (older Windows / clones).
    const char* dlls[] = {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"};
    for (auto d : dlls) {
        xinput_ = LoadLibraryA(d);
        if (!xinput_) continue;
        pGetState_ = reinterpret_cast<XIGetState>(GetProcAddress(xinput_, "XInputGetState"));
        pSetState_ = reinterpret_cast<XISetState>(GetProcAddress(xinput_, "XInputSetState"));
        if (pGetState_) break;
        FreeLibrary(xinput_);
        xinput_ = nullptr;
    }
}

InputManager::~InputManager() {
    if (xinput_) FreeLibrary(xinput_);
}

bool InputManager::pollXInput(int slot, PadState& out) {
    if (!pGetState_) return false;
    XINPUT_STATE st{};
    if (pGetState_(slot, &st) != ERROR_SUCCESS) return false;
    const auto& g = st.Gamepad;
    out.connected = true;
    out.api = "XInput";
    out.slot = slot;
    out.a = (g.wButtons & XINPUT_GAMEPAD_A) != 0;
    out.b = (g.wButtons & XINPUT_GAMEPAD_B) != 0;
    out.x = (g.wButtons & XINPUT_GAMEPAD_X) != 0;
    out.y = (g.wButtons & XINPUT_GAMEPAD_Y) != 0;
    out.lb = (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    out.rb = (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    out.menu = (g.wButtons & XINPUT_GAMEPAD_START) != 0;
    out.view = (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
    out.l3 = (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    out.r3 = (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    out.dup = (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    out.ddown = (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    out.dleft = (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    out.dright = (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    out.lx = g.sThumbLX; out.ly = g.sThumbLY;
    out.rx = g.sThumbRX; out.ry = g.sThumbRY;
    out.lt = g.bLeftTrigger; out.rt = g.bRightTrigger;
    return true;
}

bool InputManager::pollWinMM(int id, PadState& out) {
    JOYINFOEX ji{sizeof(ji), JOY_RETURNALL};
    if (joyGetPosEx(id, &ji) != JOYERR_NOERROR) return false;
    JOYCAPSA cap{};
    if (joyGetDevCapsA(id, &cap, sizeof(cap)) != JOYERR_NOERROR) return false;
    out.connected = true;
    out.api = "WinMM";
    out.slot = id;
    // Generic mapping: btn0=A,1=B,2=X,3=Y,4=LB,5=RB,6=View,7=Menu,8=L3,9=R3.
    auto btn = [&](int i) { return (ji.dwButtons & (1u << i)) != 0; };
    out.a = btn(0); out.b = btn(1); out.x = btn(2); out.y = btn(3);
    out.lb = btn(4); out.rb = btn(5); out.view = btn(6); out.menu = btn(7);
    out.l3 = btn(8); out.r3 = btn(9);
    out.dup = ji.dwPOV != JOY_POVCENTERED && ji.dwPOV <= 4500 * 1 + 2250;
    out.ddown = ji.dwPOV != JOY_POVCENTERED && ji.dwPOV >= 13500 - 2250 && ji.dwPOV <= 22500 + 2250;
    out.dleft = ji.dwPOV != JOY_POVCENTERED && ji.dwPOV >= 22500 - 2250;
    out.dright = ji.dwPOV != JOY_POVCENTERED && ji.dwPOV <= 13500 + 2250 && ji.dwPOV >= 4500 - 2250;
    auto norm = [](DWORD v) { return (int)((v - 32768) > 32767 ? 32767 : ((int)v - 32768)); };
    out.lx = norm(ji.dwXpos); out.ly = norm(ji.dwYpos);
    out.rx = norm(ji.dwRpos); out.ry = norm(ji.dwZpos);
    out.lt = (int)(ji.dwVpos >> 8); out.rt = (int)(ji.dwUpos >> 8);
    return true;
}

void InputManager::Poll() {
    primary_ = PadState{};
    for (int i = 0; i < 4; ++i) {
        slots_[i] = PadState{};
        if (pollXInput(i, slots_[i])) {
            if (!primary_.connected) primary_ = slots_[i];
        }
    }
    if (!primary_.connected) {
        PadState w{};
        for (int id = 0; id < 16; ++id) {
            if (pollWinMM(id, w)) { primary_ = w; break; }
        }
    }
}

std::vector<PadDevice> InputManager::Devices() {
    std::vector<PadDevice> ds;
    for (int i = 0; i < 4; ++i) {
        PadState s;
        bool ok = pollXInput(i, s);
        char name[64];
        snprintf(name, sizeof(name), "XInput Slot %d", i);
        ds.push_back({name, "XInput", i, ok});
    }
    for (int id = 0; id < 16; ++id) {
        JOYCAPSA cap{};
        if (joyGetDevCapsA(id, &cap, sizeof(cap)) != JOYERR_NOERROR) continue;
        JOYINFOEX ji{sizeof(ji), JOY_RETURNALL};
        bool ok = joyGetPosEx(id, &ji) == JOYERR_NOERROR;
        ds.push_back({cap.szPname, "WinMM/DirectInput", id, ok});
    }
    return ds;
}

bool InputManager::AnyConnected() { return primary_.connected; }

InputManager::NavEvent InputManager::ConsumeNav() {
    NavEvent e;
    if (!primary_.connected) { prevButtons_ = 0; return e; }
    DWORD b = 0;
    if (primary_.a) b |= 1; if (primary_.b) b |= 2;
    if (primary_.dup) b |= 4; if (primary_.ddown) b |= 8;
    if (primary_.dleft) b |= 16; if (primary_.dright) b |= 32;
    if (primary_.lb) b |= 64; if (primary_.rb) b |= 128;
    if (primary_.menu) b |= 256; if (primary_.view) b |= 512;
    // Left stick as dpad with debounce + repeat.
    int dir = 0;
    if (primary_.ly > 16000) dir = 1; else if (primary_.ly < -16000) dir = 2;
    else if (primary_.lx < -16000) dir = 3; else if (primary_.lx > 16000) dir = 4;
    DWORD now = GetTickCount();
    DWORD pressed = b & ~prevButtons_;
    prevButtons_ = b;
    auto repeatOk = [&] {
        if (now - lastRepeatMs_ > 220) { lastRepeatMs_ = now; return true; }
        return false;
    };
    if (pressed & 1) e.type = NavEvent::Confirm;
    else if (pressed & 2) e.type = NavEvent::Back;
    else if (pressed & 4) { e.type = NavEvent::Up; lastRepeatMs_ = now; lastStickDir_ = 0; }
    else if (pressed & 8) { e.type = NavEvent::Down; lastRepeatMs_ = now; lastStickDir_ = 0; }
    else if (pressed & 16) e.type = NavEvent::Left;
    else if (pressed & 32) e.type = NavEvent::Right;
    else if (pressed & 64) e.type = NavEvent::TabLeft;
    else if (pressed & 128) e.type = NavEvent::TabRight;
    else if (pressed & 256) e.type = NavEvent::Menu;
    else if (pressed & 512) e.type = NavEvent::View;
    else if (dir && dir != lastStickDir_) {
        lastStickDir_ = dir; lastRepeatMs_ = now;
        e.type = dir == 1 ? NavEvent::Up : dir == 2 ? NavEvent::Down : dir == 3 ? NavEvent::Left : NavEvent::Right;
    } else if (dir && repeatOk()) {
        e.type = dir == 1 ? NavEvent::Up : dir == 2 ? NavEvent::Down : dir == 3 ? NavEvent::Left : NavEvent::Right;
    }
    if (!dir && !(b & (4 | 8))) lastStickDir_ = 0;
    (void)lastNavMs_;
    return e;
}

bool InputManager::RumbleTest(int ms) {
    if (!pSetState_ || !primary_.connected || primary_.api != "XInput") return false;
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = 45000;
    v.wRightMotorSpeed = 45000;
    if (pSetState_(primary_.slot, &v) != ERROR_SUCCESS) return false;
    Sleep(ms > 0 ? ms : 800);
    XINPUT_VIBRATION stop{};
    pSetState_(primary_.slot, &stop);
    return true;
}

const char* InputManager::SchemeText(bool swapAB) {
    return swapAB
        ? "A -> Circle(O) | B -> Cross(X) | X -> Square | Y -> Triangle\nLB/RB -> L1/R1 | LT/RT -> L2/R2 (analog)\nMenu -> Options | View -> TouchPad | D-pad/sticks 1:1"
        : "A -> Cross(X) | B -> Circle(O) | X -> Square | Y -> Triangle\nLB/RB -> L1/R1 | LT/RT -> L2/R2 (analog)\nMenu -> Options | View -> TouchPad | D-pad/sticks 1:1";
}

} // namespace launcher
