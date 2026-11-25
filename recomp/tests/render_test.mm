// Draws the same Direct3D 9 scene through the game's API path (the COM shims) with the
// software renderer and with the Metal renderer, writes both frames as PNG and compares
// them. No game data: the scene is synthetic (textured blended quads, a lit depth-tested
// cube, a vs_1_1/ps_1_1 draw, alpha test, fog).
//   render_test <out dir>          (macOS; without a Metal device only the soft frame)
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "d3d9.h"
#include "metal.h"
#include "runtime.h"
#include "soft.h"

using namespace rt;

namespace {
CPU *cpu;

uint32_t vcall(uint32_t obj, int index, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t fn = ld32(ld32(obj) + 4 * uint32_t(index));
    switch (a.size()) {
    case 1: return call_guest(cpu, fn, {a[0]});
    case 2: return call_guest(cpu, fn, {a[0], a[1]});
    case 3: return call_guest(cpu, fn, {a[0], a[1], a[2]});
    case 4: return call_guest(cpu, fn, {a[0], a[1], a[2], a[3]});
    case 5: return call_guest(cpu, fn, {a[0], a[1], a[2], a[3], a[4]});
    case 6: return call_guest(cpu, fn, {a[0], a[1], a[2], a[3], a[4], a[5]});
    case 7: return call_guest(cpu, fn, {a[0], a[1], a[2], a[3], a[4], a[5], a[6]});
    case 9: return call_guest(cpu, fn, {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]});
    default: fatal("vcall arity %zu", a.size());
    }
}
uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
uint32_t gbytes(const void *p, size_t n) {
    uint32_t a = galloc(uint32_t(n), true);
    memcpy(GM + a, p, n);
    return a;
}
uint32_t matrix(const float m[16]) { return gbytes(m, 64); }

// IDirect3DDevice9 method numbers
enum {
    PRESENT = 17, CREATETEXTURE = 23, BEGINSCENE = 41, ENDSCENE = 42, CLEAR = 43, SETTRANSFORM = 44,
    SETMATERIAL = 49, SETLIGHT = 51, LIGHTENABLE = 53, SETRENDERSTATE = 57, SETTEXTURE = 65, SETTSS = 67,
    SETSAMPLER = 69, DRAWUP = 83, CREATEDECL = 86, SETDECL = 87, SETFVF = 89, CREATEVS = 91, SETVS = 92,
    SETVSCONST = 94, CREATEPS = 106, SETPS = 107,
};

uint32_t checker_texture(uint32_t dev) {
    uint32_t out = galloc(4, true);
    vcall(dev, CREATETEXTURE, {64, 64, 1, 0, 21, 1, out, 0});
    uint32_t tex = ld32(out);
    uint32_t locked = galloc(8, true);
    vcall(tex, 19, {0, locked, 0, 0});  // LockRect
    uint32_t pitch = ld32(locked), bits = ld32(locked + 4);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++) {
            bool on = ((x / 8) ^ (y / 8)) & 1;
            uint32_t a = 0x80 + (x * 2);
            st32(bits + y * pitch + 4 * x, (a << 24) | (on ? 0xFFD040 : 0x2060E0));
        }
    vcall(tex, 20, {0});  // UnlockRect
    return tex;
}

void draw_scene(uint32_t dev) {
    vcall(dev, CLEAR, {0, 0, 3, 0xFF304050, fbits(1.0f), 0});
    vcall(dev, BEGINSCENE, {});
    // 1. A lit, depth-tested cube (fixed function, XYZ | NORMAL).
    float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 5, 1};
    float f = 1.0f / std::tan(0.5f), zn = 1, zf = 100;
    float proj[16] = {f * 0.75f, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    float ca = std::cos(0.6f), sa = std::sin(0.6f), cb = std::cos(0.4f), sb = std::sin(0.4f);
    float world[16] = {ca, sa * sb, -sa * cb, 0, 0, cb, sb, 0, sa, -ca * sb, ca * cb, 0, -1.2f, 0.2f, 0, 1};
    vcall(dev, SETTRANSFORM, {256, matrix(world)});
    vcall(dev, SETTRANSFORM, {2, matrix(view)});
    vcall(dev, SETTRANSFORM, {3, matrix(proj)});
    float light[26] = {0};
    uint32_t *lu = reinterpret_cast<uint32_t *>(light);
    lu[0] = 3;
    light[1] = light[2] = light[3] = light[4] = 1;      // diffuse
    light[9] = light[10] = light[11] = 0.2f;            // ambient
    light[16] = 0.3f, light[17] = -0.5f, light[18] = 1;  // direction
    vcall(dev, SETLIGHT, {0, gbytes(light, 104)});
    vcall(dev, LIGHTENABLE, {0, 1});
    float mat[17] = {1, 0.4f, 0.4f, 1, 1, 0.4f, 0.4f, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    vcall(dev, SETMATERIAL, {gbytes(mat, 68)});
    vcall(dev, SETRENDERSTATE, {137, 1});   // lighting
    vcall(dev, SETRENDERSTATE, {7, 1});     // z enable
    vcall(dev, SETRENDERSTATE, {22, 1});    // no culling
    vcall(dev, SETTEXTURE, {0, 0});
    vcall(dev, SETTSS, {0, 1, 3});          // colour = diffuse (SELECTARG2 of TEXTURE, CURRENT)
    vcall(dev, SETTSS, {0, 4, 3});
    std::vector<float> cube;
    const float n[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
    for (int face = 0; face < 6; face++) {
        float nx = n[face][0], ny = n[face][1], nz = n[face][2];
        float ux = ny != 0 ? 1.0f : 0.0f, uy = 0, uz = ny != 0 ? 0.0f : 1.0f;
        if (nz != 0) ux = 1, uz = 0;
        float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;
        float c[4][3];
        int s[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < 3; i++) {
                float nn[3] = {nx, ny, nz}, uu[3] = {ux, uy, uz}, vv[3] = {vx, vy, vz};
                c[k][i] = nn[i] + s[k][0] * uu[i] + s[k][1] * vv[i];
            }
        int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int k : idx) cube.insert(cube.end(), {c[k][0] * 0.8f, c[k][1] * 0.8f, c[k][2] * 0.8f, nx, ny, nz});
    }
    vcall(dev, SETFVF, {0x12});
    vcall(dev, DRAWUP, {4, 12, gbytes(cube.data(), cube.size() * 4), 24});

    // 2. A vs_1_1 / ps_1_1 textured quad (declaration: position, texcoord).
    uint32_t tex = checker_texture(dev);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0,   0, 0, 12, 0, 1, 0, 5, 0,   0xFF, 0, 0, 0, 17, 0, 0, 0};
    uint32_t out = galloc(4, true);
    vcall(dev, CREATEDECL, {gbytes(decl, sizeof decl), out});
    uint32_t vdecl = ld32(out);
    const uint32_t vs[] = {0xFFFE0101, 0x1F, 0x80000000, 0x900F0000, 0x1F, 0x80000005, 0x900F0001,
                           0x14, 0xC00F0000, 0x90E40000, 0xA0E40000,   // m4x4 oPos, v0, c0
                           0x01, 0xE0030000, 0x90E40001,               // mov oT0.xy, v1
                           0x01, 0xD00F0000, 0xA0E40004,               // mov oD0, c4
                           0x0000FFFF};
    const uint32_t ps[] = {0xFFFF0101, 0x42, 0xB00F0000,                 // tex t0
                           0x05, 0x800F0000, 0xB0E40000, 0x90E40000,     // mul r0, t0, v0
                           0x0000FFFF};
    vcall(dev, CREATEVS, {gbytes(vs, sizeof vs), out});
    uint32_t vsh = ld32(out);
    vcall(dev, CREATEPS, {gbytes(ps, sizeof ps), out});
    uint32_t psh = ld32(out);
    // c0..c3: world*view*proj transposed (rows for dp4); c4: colour.
    float wvp[16] = {0};
    float wq[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.3f, -0.1f, 0, 1};
    float t1[16], t2[16];
    auto mul = [](const float *a, const float *b, float *o) {
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) {
                o[4 * i + j] = 0;
                for (int k = 0; k < 4; k++) o[4 * i + j] += a[4 * i + k] * b[4 * k + j];
            }
    };
    mul(wq, view, t1);
    mul(t1, proj, t2);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) wvp[4 * i + j] = t2[4 * j + i];
    float consts[20];
    memcpy(consts, wvp, 64);
    consts[16] = 1, consts[17] = 1, consts[18] = 1, consts[19] = 1;
    vcall(dev, SETVSCONST, {0, gbytes(consts, sizeof consts), 5});
    vcall(dev, SETDECL, {vdecl});
    vcall(dev, SETVS, {vsh});
    vcall(dev, SETPS, {psh});
    vcall(dev, SETTEXTURE, {0, tex});
    vcall(dev, SETRENDERSTATE, {137, 0});
    float quad[] = {-1, -1, 0, 0, 1, 1, -1, 0, 1, 1, -1, 1, 0, 0, 0, 1, 1, 0, 1, 0};
    vcall(dev, DRAWUP, {5, 2, gbytes(quad, sizeof quad), 20});
    vcall(dev, SETVS, {0});
    vcall(dev, SETPS, {0});
    vcall(dev, SETDECL, {0});

    // 3. Pre-transformed quads (XYZRHW | DIFFUSE | TEX1): textured with alpha blending, and
    // with the alpha test.
    vcall(dev, SETRENDERSTATE, {7, 0});
    vcall(dev, SETRENDERSTATE, {27, 1});  // blending
    vcall(dev, SETRENDERSTATE, {19, 5});
    vcall(dev, SETRENDERSTATE, {20, 6});
    vcall(dev, SETTSS, {0, 1, 4});        // modulate texture, diffuse
    vcall(dev, SETTSS, {0, 2, 2});
    vcall(dev, SETTSS, {0, 3, 0});
    vcall(dev, SETTSS, {0, 4, 4});
    vcall(dev, SETTSS, {0, 5, 2});
    vcall(dev, SETTSS, {0, 6, 0});
    vcall(dev, SETSAMPLER, {0, 5, 2});    // linear
    vcall(dev, SETSAMPLER, {0, 6, 2});
    struct V {
        float x, y, z, rhw;
        uint32_t c;
        float u, v;
    };
    auto rect = [&](float x0, float y0, float x1, float y1, uint32_t col) {
        V q[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, col, 0, 0}, {x1 - 0.5f, y0 - 0.5f, 0, 1, col, 1, 0},
                  {x0 - 0.5f, y1 - 0.5f, 0, 1, col, 0, 1}, {x1 - 0.5f, y1 - 0.5f, 0, 1, col, 1, 1}};
        vcall(dev, SETFVF, {0x144});
        vcall(dev, DRAWUP, {5, 2, gbytes(q, sizeof q), 28});
    };
    rect(40, 40, 296, 296, 0xFFFFFFFF);
    vcall(dev, SETRENDERSTATE, {15, 1});   // alpha test
    vcall(dev, SETRENDERSTATE, {24, 0xC0});
    vcall(dev, SETRENDERSTATE, {25, 7});   // GREATEREQUAL
    rect(520, 340, 760, 580, 0xFFFF8080);
    vcall(dev, SETRENDERSTATE, {15, 0});
    vcall(dev, SETTEXTURE, {0, 0});
    vcall(dev, SETTSS, {0, 1, 3});
    vcall(dev, SETTSS, {0, 4, 3});
    rect(320, 480, 480, 560, 0x8000FF00);  // half-transparent green
    vcall(dev, ENDSCENE, {});
    vcall(dev, PRESENT, {0, 0, 0, 0});
}

uint32_t make_device() {
    uint32_t create = resolve_import("d3d9.dll", "Direct3DCreate9");
    uint32_t d3d = call_guest(cpu, create, {32});
    uint32_t pp = galloc(56, true);
    st32(pp, 800);
    st32(pp + 4, 600);
    st32(pp + 8, 22);
    st32(pp + 12, 1);
    st32(pp + 24, 1);
    st32(pp + 32, 1);
    st32(pp + 36, 1);
    st32(pp + 40, 80);
    uint32_t out = galloc(4, true);
    vcall(d3d, 16, {0, 1, 0x10010, 0x40, pp, out});
    return ld32(out);
}

bool frame(uint32_t dev_obj, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h, d3d::Renderer *r) {
    auto dev = com_as<d3d::Device>(dev_obj);
    return dev && r->read_frame(*dev, px, w, h);
}
}  // namespace

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    memory_init();
    cpu = new_cpu(kMainStack);

    d3d::SoftOptions so;
    so.dir = out;
    so.every = 1;
    auto soft = d3d::make_soft_renderer(so);
    d3d::Renderer *soft_r = soft.get();
    d3d::set_renderer(std::move(soft));
    uint32_t d1 = make_device();
    draw_scene(d1);
    std::vector<uint32_t> a, b;
    uint32_t w = 0, h = 0, w2 = 0, h2 = 0;
    if (!frame(d1, a, w, h, soft_r)) fatal("no soft frame");
    d3d::save_png(out + "/soft.png", w, h, a);

    id<MTLDevice> mdev = MTLCreateSystemDefaultDevice();
    if (!mdev) {
        printf("no Metal device: soft frame only\n");
        return 0;
    }
    printf("Metal device: %s\n", mdev.name.UTF8String);
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = mdev;
    layer.drawableSize = CGSizeMake(800, 600);
    auto metal = d3d::make_metal_renderer((__bridge void *)layer);
    d3d::Renderer *metal_r = metal.get();
    d3d::set_renderer(std::move(metal));
    uint32_t d2 = make_device();
    draw_scene(d2);
    if (!frame(d2, b, w2, h2, metal_r)) fatal("no Metal frame");
    d3d::save_png(out + "/metal.png", w2, h2, b);

    // Compare: channels within 12, and at most 1.5% of pixels different (edges are
    // rasterised a little differently).
    size_t bad = 0;
    std::vector<uint32_t> diff(a.size());
    for (size_t i = 0; i < a.size() && i < b.size(); i++) {
        int worst = 0;
        for (int s = 0; s < 24; s += 8) worst = std::max(worst, std::abs(int((a[i] >> s) & 0xFF) - int((b[i] >> s) & 0xFF)));
        if (worst > 12) bad++;
        diff[i] = worst > 12 ? 0xFFFF0000 : (a[i] & 0xFEFEFE) >> 1;
    }
    d3d::save_png(out + "/diff.png", w, h, diff);
    double pct = 100.0 * double(bad) / double(a.size());
    printf("%zu of %zu pixels differ (%.2f%%)\n", bad, a.size(), pct);
    return pct <= 1.5 ? 0 : 1;
}
