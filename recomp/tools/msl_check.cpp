// Writes Metal Shading Language for CI to compile with Apple's `metal` compiler: the shared
// prelude and fixed-function functions, and translations of synthetic vs_1_1 / ps_1_1
// programs that use the instructions and modifiers the game's shaders use (no game data).
//   msl_check <out dir>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "msl.h"
#include "shader.h"

namespace {
// Token helpers (Direct3D 9 shader bytecode).
uint32_t reg(uint32_t type, uint32_t num) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | num; }
uint32_t dst(uint32_t type, uint32_t num, uint32_t mask = 0xF, bool sat = false, int shift = 0) {
    return reg(type, num) | (mask << 16) | (sat ? 0x100000u : 0) | (uint32_t(shift & 0xF) << 24);
}
uint32_t src(uint32_t type, uint32_t num, uint32_t swz = 0xE4, uint32_t mod = 0) {
    return reg(type, num) | (swz << 16) | (mod << 24);
}
enum { T = 0, V = 1, C = 2, A = 3, RAST = 4, ATTR = 5, TEXC = 6 };
uint32_t f(float x) {
    uint32_t u;
    memcpy(&u, &x, 4);
    return u;
}

const std::vector<uint32_t> kVertex = {
    0xFFFE0101,
    0x1F, 0x80000000, dst(V, 0),                              // dcl_position v0
    0x1F, 0x80000003, dst(V, 1),                              // dcl_normal v1
    0x1F, 0x80000005, dst(V, 2),                              // dcl_texcoord v2
    0x1F, 0x80000002, dst(V, 3),                              // dcl_blendindices v3
    0x51, dst(C, 90), f(0.5f), f(1.0f), f(3.0f), f(0.0f),     // def c90
    0x01, dst(A, 0, 1), src(V, 3, 0x00),                      // mov a0.x, v3.x
    0x14, dst(T, 0), src(V, 0), src(C, 10) | 0x2000,          // m4x4 r0, v0, c[a0.x + 10]
    0x14, dst(RAST, 0), src(T, 0), src(C, 0),                 // m4x4 oPos, r0, c0
    0x17, dst(T, 1, 7), src(V, 1), src(C, 4),                 // m3x3 r1.xyz, v1, c4
    0x08, dst(T, 2, 8), src(T, 1), src(T, 1),                 // dp3 r2.w, r1, r1
    0x07, dst(T, 2, 8), src(T, 2, 0xFF),                      // rsq r2.w, r2.w
    0x05, dst(T, 1, 7), src(T, 1), src(T, 2, 0xFF),           // mul r1.xyz, r1, r2.w
    0x10, dst(T, 3), src(T, 1),                               // lit r3, r1
    0x04, dst(ATTR, 0), src(T, 3, 0x55), src(C, 7), src(C, 8),  // mad oD0, r3.y, c7, c8
    0x0B, dst(T, 4), src(T, 1), src(C, 90, 0x00, 1),          // max r4, r1, -c90.x
    0x0C, dst(T, 5), src(T, 4), src(C, 90),                   // slt r5, r4, c90
    0x0D, dst(T, 6), src(T, 4), src(C, 90),                   // sge r6, r4, c90
    0x06, dst(T, 7, 8), src(T, 0, 0xFF),                      // rcp r7.w, r0.w
    0x0E, dst(T, 7, 1), src(T, 7, 0xFF),                      // exp r7.x, r7.w
    0x0F, dst(T, 7, 2), src(T, 7, 0xFF),                      // log r7.y, r7.w
    0x13, dst(T, 8), src(T, 7),                               // frc r8, r7
    0x11, dst(T, 9), src(T, 7), src(T, 8),                    // dst r9, r7, r8
    0x4E, dst(T, 10), src(T, 9, 0xFF),                        // expp r10, r9.w
    0x01, dst(RAST, 1, 1), src(T, 10, 0x00),                  // mov oFog, r10.x
    0x01, dst(TEXC, 0, 3), src(V, 2),                         // mov oT0.xy, v2
    0x01, dst(ATTR, 1), src(T, 5),                            // mov oD1, r5
    0x0000FFFF,
};

const std::vector<uint32_t> kPixel = {
    0xFFFF0101,
    0x51, dst(C, 7), f(0.1f), f(0.2f), f(0.3f), f(0.5f),          // def c7
    0x42, dst(A, 0),                                              // tex t0
    0x42, dst(A, 1),                                              // tex t1
    0x05, dst(T, 0), src(A, 0), src(V, 0),                        // mul r0, t0, v0
    0x03, dst(T, 0, 8), src(V, 1), src(A, 1, 0xE4, 2),            // sub r0.a, v1, t1_bias
    0x50, dst(T, 1), src(T, 0, 0xFF), src(C, 7), src(C, 1),       // cnd r1, r0.a, c7, c1
    0x02, dst(T, 0, 7, true), src(T, 0), src(T, 1, 0xE4, 6),      // add_sat r0.rgb, r0, 1-r1
    0x08, dst(T, 1, 0xF, false, 1), src(A, 0, 0xE4, 4), src(C, 2),  // dp3_x2 r1, t0_bx2, c2
    0x12, dst(T, 0), src(T, 1, 0xFF), src(T, 0), src(A, 1),       // lrp r0, r1.a, r0, t1
    0x04, dst(T, 0, 7), src(T, 0), src(V, 0, 0xE4, 1), src(C, 3),  // mad r0.rgb, r0, -v0, c3
    0x0000FFFF,
};
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    std::ofstream(dir + "/common.metal") << rt::msl::prelude() << rt::msl::fixed_function();
    int fails = 0;
    struct Case {
        const char *name;
        const std::vector<uint32_t> &code;
    } cases[] = {{"vs_test", kVertex}, {"ps_test", kPixel}};
    for (auto &c : cases) {
        rt::shader::Program p = rt::shader::decode(c.code.data(), c.code.size());
        std::string err;
        std::string body = p.pixel ? rt::msl::fragment(p, c.name, &err) : rt::msl::vertex(p, c.name, &err);
        if (!p.error.empty() || !err.empty() || body.empty()) {
            fprintf(stderr, "%s: %s%s\n", c.name, p.error.c_str(), err.c_str());
            fails++;
            continue;
        }
        std::ofstream(dir + "/" + c.name + ".metal") << rt::msl::prelude() << body;
        printf("%s: %zu instructions\n", c.name, p.ins.size());
    }
    return fails ? 1 : 0;
}
