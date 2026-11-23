// How the game draws a stage: Stage::Draw 0x406440 and the device defaults set at
// 0x5D6500 (analysis/05-stage-loading.md, "Stage draw"). Shared by the Metal renderer
// (through daybreak_c.h) and the CPU reference renderer (tools/render_stage.cpp).
//
// Device defaults (0x5D6500): LIGHTING off, so a pixel is texture x vertex colour
// (TSS0 MODULATE for colour and alpha; white without vertex colours). ALPHABLENDENABLE on
// with SRCALPHA / INVSRCALPHA, alpha test GREATEREQUAL 1, CULLMODE CCW (front faces are
// clockwise on screen), linear sampling, linear table fog. Projection at init:
// perspective LH, fov pi/4.
//
// Not reproduced yet: render mode 3 (a projected second texture on slots 1, 3 and 5 when
// Stage+0x2C is set; its texture and matrix were not traced) and the second pass at
// 0x406740 (slots 6, 5, 7; its timing against characters is unknown). Slot 7 is drawn
// at the end with Z-write off, as that pass does.
#pragma once
#include <cstdint>

namespace db {

enum class Blend : int32_t {
    Alpha = 0,     // SRCALPHA, INVSRCALPHA
    Multiply = 1,  // DESTCOLOR, ZERO (slot 4: shadow / light layer)
    Additive = 2,  // SRCALPHA, ONE (any subset whose material Power != 0, 0x5D1750)
};

struct Pass {
    int32_t slot;
    Blend blend;
    bool z_write;
    bool fog;
    bool alpha_test;      // discard alpha < 1/255
    bool point_sampling;  // else linear
};

// In draw order.
inline const Pass kStagePasses[] = {
    {1, Blend::Alpha, true, true, false, false},
    {5, Blend::Alpha, true, true, false, false},
    {4, Blend::Multiply, false, false, false, false},
    {3, Blend::Alpha, true, true, false, false},
    {6, Blend::Alpha, true, true, true, true},
    {2, Blend::Alpha, true, true, true, true},
    {0, Blend::Alpha, true, false, false, false},  // sky dome, no fog
    {7, Blend::Alpha, false, true, false, false},
};
inline constexpr int kStagePassCount = int(sizeof kStagePasses / sizeof kStagePasses[0]);

// A subset with material Power != 0 overrides its pass: additive, no Z-write, no fog.
inline Pass subset_pass(const Pass& p, float power) {
    if (power == 0) return p;
    Pass q = p;
    q.blend = Blend::Additive;
    q.z_write = false;
    q.fog = false;
    return q;
}

}  // namespace db
