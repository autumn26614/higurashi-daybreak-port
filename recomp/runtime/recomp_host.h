// What the iOS app calls (C, for Swift).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 when the app was built without the translated game code.
int recomp_has_game(void);
// 1 if `dir` holds daybreak.exe, d3dx9_25.dll and daybreak00/01/02.dat.
int recomp_files_present(const char *dir);
// Starts the game on its own thread, drawing into `layer` (a CAMetalLayer*). Once only.
void recomp_start(const char *dir, void *layer);
// Keyboard input as DirectInput key codes (DIK_*).
void recomp_key(uint32_t dik, int down);
// Pulls `frames` stereo frames of 16-bit sound at 44.1 kHz from the mixer.
void recomp_audio(int16_t *out, int frames);

#ifdef __cplusplus
}
#endif
