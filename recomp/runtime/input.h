// Input the host feeds to the game (DirectInput key codes, DIK_*).
#pragma once

#include <cstdint>

namespace rt {
namespace input {

void set_key(uint32_t dik, bool down);
bool key(uint32_t dik);

// A game controller, as DirectInput reports it (axes -1..1, buttons as bits, POV in
// degrees clockwise from up or -1 when centred). The game looks for controllers at start.
struct Joystick {
    bool connected = false;
    float x = 0, y = 0, z = 0, rx = 0, ry = 0, rz = 0;
    uint32_t buttons = 0;
    int pov = -1;
};
void set_joystick(const Joystick &j);
Joystick joystick();

// DIK codes the game's default keys use.
enum : uint32_t {
    DIK_ESCAPE = 0x01, DIK_RETURN = 0x1C, DIK_SPACE = 0x39, DIK_Z = 0x2C, DIK_X = 0x2D, DIK_C = 0x2E,
    DIK_A = 0x1E, DIK_S = 0x1F, DIK_D = 0x20, DIK_UP = 0xC8, DIK_DOWN = 0xD0, DIK_LEFT = 0xCB, DIK_RIGHT = 0xCD,
};

}  // namespace input
}  // namespace rt
