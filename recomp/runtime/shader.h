// Direct3D 9 shader bytecode (vs_1_1 .. vs_2_0, ps_1_1 .. ps_1_4): decoding, and an
// interpreter for the software renderer. The Metal backend generates code from the same
// decoded form.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rt {
namespace shader {

enum RegType : uint8_t {
    TEMP = 0, INPUT = 1, CONST = 2, ADDR_OR_TEX = 3, RASTOUT = 4, ATTROUT = 5, TEXCRDOUT = 6, CONSTINT = 7,
    COLOROUT = 8, DEPTHOUT = 9, SAMPLER = 10, CONSTBOOL = 14,
};

enum Op : uint16_t {
    NOP = 0, MOV = 1, ADD = 2, SUB = 3, MAD = 4, MUL = 5, RCP = 6, RSQ = 7, DP3 = 8, DP4 = 9, MIN = 10, MAX = 11,
    SLT = 12, SGE = 13, EXP = 14, LOG = 15, LIT = 16, DST = 17, LRP = 18, FRC = 19, M4x4 = 20, M4x3 = 21,
    M3x4 = 22, M3x3 = 23, M3x2 = 24, DCL = 31, POW = 32, ABS = 35, NRM = 36, SINCOS = 37, MOVA = 46,
    TEXCOORD = 64, TEXKILL = 65, TEX = 66, TEXBEM = 67, TEXBEML = 68, TEXREG2AR = 69, TEXREG2GB = 70,
    TEXM3x2PAD = 71, TEXM3x2TEX = 72, TEXM3x3PAD = 73, TEXM3x3TEX = 74, TEXM3x3SPEC = 76, TEXM3x3VSPEC = 77,
    EXPP = 78, LOGP = 79, CND = 80, DEF = 81, CMP = 88, BEM = 89, DP2ADD = 90, PHASE = 0xFFFD,
};

struct Src {
    uint8_t type = 0;
    uint16_t num = 0;
    bool relative = false;  // c[a0.x + num]
    uint8_t swz[4] = {0, 1, 2, 3};
    uint8_t mod = 0;        // D3DSPSM_*: 1 neg, 2 bias, 3 bias neg, 4 bx2, 5 bx2 neg, 6 comp, 7 x2, 8 x2 neg, 11 abs, 12 abs neg
};

struct Dst {
    uint8_t type = 0;
    uint16_t num = 0;
    uint8_t mask = 0xF;
    bool sat = false;
    int8_t shift = 0;  // result scale: 1 = x2, 2 = x4, -1 = d2, ...
};

struct Ins {
    uint16_t op = NOP;
    bool coissue = false;
    Dst d;
    Src s[3];
    int nsrc = 0;
    uint32_t usage = 0, usage_index = 0;  // DCL
    float def[4] = {0, 0, 0, 0};          // DEF
};

struct Program {
    bool pixel = false;
    uint8_t major = 0, minor = 0;
    std::vector<Ins> ins;
    // vs inputs: v# -> (usage, usage index) from the dcl instructions
    std::vector<std::array<uint32_t, 3>> inputs;  // reg, usage, index
    std::vector<std::pair<uint32_t, std::array<float, 4>>> defs;  // c# constants the shader defines
    std::string error;  // non-empty: could not decode
};

Program decode(const uint32_t *code, size_t tokens);

using Vec4 = std::array<float, 4>;

struct VSIn {
    Vec4 v[16];
};
struct VSOut {
    Vec4 pos{0, 0, 0, 1};
    Vec4 color[2]{{1, 1, 1, 1}, {0, 0, 0, 0}};
    Vec4 tex[8]{};
    float fog = 1.0f, psize = 1.0f;
};
// Runs a vertex shader for one vertex; `consts` is the device's 256 float constants.
void run_vs(const Program &p, const float *consts, const VSIn &in, VSOut &out);

// Pixel shader inputs: interpolated colours and texture coordinates, and a sampler.
struct PSIn {
    Vec4 color[2];
    Vec4 tex[8];
};
using Sampler = Vec4 (*)(void *ctx, int stage, float u, float v);
// Returns false if the pixel is killed (texkill).
bool run_ps(const Program &p, const float *consts, const PSIn &in, Sampler sample, void *ctx, Vec4 &out);

}  // namespace shader
}  // namespace rt
