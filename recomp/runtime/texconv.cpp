// Texture formats to RGBA, for the renderers.
#include "texconv.h"

#include <cmath>
#include <cstring>

namespace rt {
namespace d3d {

namespace {
using shader::Vec4;
Vec4 color_of(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
            ((argb >> 24) & 0xFF) / 255.0f};
}
void decode_dxt_block(const uint8_t *b, uint32_t fmt, Vec4 out[16]) {
    const uint8_t *cb = fmt == FMT_DXT1 ? b : b + 8;
    uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
    auto rgb = [](uint16_t c) {
        return Vec4{((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f, 1.0f};
    };
    Vec4 p[4] = {rgb(c0), rgb(c1), {}, {}};
    if (fmt != FMT_DXT1 || c0 > c1) {
        for (int i = 0; i < 4; i++) {
            p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
            p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
        }
    } else {
        for (int i = 0; i < 4; i++) p[2][i] = (p[0][i] + p[1][i]) / 2;
        p[3] = {0, 0, 0, 0};
    }
    uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
    for (int i = 0; i < 16; i++) out[i] = p[(bits >> (2 * i)) & 3];
    if (fmt == FMT_DXT2 || fmt == FMT_DXT3) {
        for (int i = 0; i < 16; i++) out[i][3] = ((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
    } else if (fmt == FMT_DXT4 || fmt == FMT_DXT5) {
        float a[8];
        a[0] = b[0] / 255.0f;
        a[1] = b[1] / 255.0f;
        if (b[0] > b[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else {
            for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
            a[6] = 0;
            a[7] = 1;
        }
        uint64_t ab = 0;
        for (int i = 0; i < 6; i++) ab |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) out[i][3] = a[(ab >> (3 * i)) & 7];
    }
}

}  // namespace

void decode_rgba(const Image &im, std::vector<Vec4> &px) {
    struct {
        uint32_t w, h;
        std::vector<Vec4> &px;
    } d{im.width, im.height, px};
    d.px.assign(size_t(d.w) * d.h, Vec4{1, 0, 1, 1});
    const uint8_t *base = GM + im.mem;
    if (is_dxt(im.format)) {
        for (uint32_t by = 0; by < (d.h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (d.w + 3) / 4; bx++) {
                Vec4 blk[16];
                decode_dxt_block(base + by * im.pitch + bx * (im.format == FMT_DXT1 ? 8 : 16), im.format, blk);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + uint32_t(i % 4), y = by * 4 + uint32_t(i / 4);
                    if (x < d.w && y < d.h) d.px[size_t(y) * d.w + x] = blk[i];
                }
            }
        return;
    }
    for (uint32_t y = 0; y < d.h; y++) {
        const uint8_t *row = base + size_t(y) * im.pitch;
        for (uint32_t x = 0; x < d.w; x++) {
            Vec4 &o = d.px[size_t(y) * d.w + x];
            switch (im.format) {
            case FMT_A8R8G8B8: case FMT_X8R8G8B8: {
                uint32_t v;
                memcpy(&v, row + 4 * x, 4);
                o = color_of(im.format == FMT_X8R8G8B8 ? v | 0xFF000000 : v);
                break;
            }
            case FMT_A8B8G8R8: case FMT_X8B8G8R8: {
                const uint8_t *p = row + 4 * x;
                o = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, im.format == FMT_X8B8G8R8 ? 1.0f : p[3] / 255.0f};
                break;
            }
            case FMT_R5G6B5: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 11) & 31) / 31.0f, ((v >> 5) & 63) / 63.0f, (v & 31) / 31.0f, 1};
                break;
            }
            case FMT_A1R5G5B5: case FMT_X1R5G5B5: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 10) & 31) / 31.0f, ((v >> 5) & 31) / 31.0f, (v & 31) / 31.0f,
                     im.format == FMT_X1R5G5B5 || (v & 0x8000) ? 1.0f : 0.0f};
                break;
            }
            case FMT_A4R4G4B4: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 8) & 15) / 15.0f, ((v >> 4) & 15) / 15.0f, (v & 15) / 15.0f, ((v >> 12) & 15) / 15.0f};
                break;
            }
            case FMT_A8: o = {0, 0, 0, row[x] / 255.0f}; break;
            case FMT_L8: o = {row[x] / 255.0f, row[x] / 255.0f, row[x] / 255.0f, 1}; break;
            case FMT_A8L8: {
                float l = row[2 * x] / 255.0f;
                o = {l, l, l, row[2 * x + 1] / 255.0f};
                break;
            }
            case FMT_R8G8B8: o = {row[3 * x + 2] / 255.0f, row[3 * x + 1] / 255.0f, row[3 * x] / 255.0f, 1}; break;
            default: break;
            }
        }
    }
}

void decode_bgra8(const Image &im, std::vector<uint32_t> &out) {
    out.resize(size_t(im.width) * im.height);
    if (im.format == FMT_A8R8G8B8 || im.format == FMT_X8R8G8B8) {
        uint32_t force = im.format == FMT_X8R8G8B8 ? 0xFF000000u : 0;
        for (uint32_t y = 0; y < im.height; y++) {
            const uint8_t *row = GM + im.mem + size_t(y) * im.pitch;
            uint32_t *o = &out[size_t(y) * im.width];
            memcpy(o, row, size_t(im.width) * 4);
            if (force)
                for (uint32_t x = 0; x < im.width; x++) o[x] |= force;
        }
        return;
    }
    std::vector<Vec4> px;
    decode_rgba(im, px);
    auto b = [](float x) { return uint32_t(std::lround(std::fmin(1.0f, std::fmax(0.0f, x)) * 255.0f)); };
    for (size_t i = 0; i < px.size(); i++) out[i] = b(px[i][3]) << 24 | b(px[i][0]) << 16 | b(px[i][1]) << 8 | b(px[i][2]);
}

}  // namespace d3d
}  // namespace rt
