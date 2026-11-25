// What the iOS app calls (C, for Swift).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// `kai`: 0 for the base game (daybreak.exe), 1 for Kai (DaybreakDX.exe).
// 0 when the app was built without that game's translated code.
int recomp_has_game(int kai);
// 1 if `dir` holds the game: its exe, d3dx9_25.dll and daybreak00-02.dat (Kai: also 04-06).
int recomp_files_present(const char *dir, int kai);
// Starts the game on its own thread, drawing into `layer` (a CAMetalLayer*). Once only.
void recomp_start(const char *dir, void *layer, int kai);
// Keyboard input as DirectInput key codes (DIK_*).
void recomp_key(uint32_t dik, int down);
// A game controller (connect it before the game starts): sticks -1..1 (y down), buttons
// in DirectInput order (XInput layout: A B X Y LB RB Back Start LS RS, then the triggers),
// POV in degrees clockwise from up or -1.
void recomp_joystick(int connected, float x, float y, float rx, float ry, uint32_t buttons, int pov);
// Pulls `frames` stereo frames of 16-bit sound at 44.1 kHz from the mixer.
void recomp_audio(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif
