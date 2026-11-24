// gdi32 text: fonts and GetGlyphOutlineA, which the game uses to draw its text into
// textures. Glyphs come from font_glyph() (CoreText on Apple platforms; elsewhere a
// placeholder box so layout can still be checked).
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "font.h"
#include "host.h"
#include "runtime.h"

using namespace rt;

namespace {
struct Font {
    int32_t height, width, weight;
    uint32_t charset;
    std::string face;
};
std::mutex font_mutex;
std::map<uint32_t, Font> fonts;
uint32_t next_font = 0x41000;
uint32_t selected = 0;

Font current_font() {
    std::lock_guard<std::mutex> lock(font_mutex);
    auto it = fonts.find(selected);
    if (it != fonts.end()) return it->second;
    return Font{16, 0, 400, 128, "MS Gothic"};
}
}  // namespace

#if !defined(__APPLE__)
namespace rt {
bool font_glyph(const FontSpec &f, uint32_t code, Glyph &g) {
    int h = f.height < 0 ? -f.height : f.height;
    if (!h) h = 16;
    bool wide = code > 0xFF;
    int adv = wide ? h : h / 2;
    g.advance = adv;
    g.ascent = h * 7 / 8;
    g.descent = h - g.ascent;
    if (code == ' ' || code == 0x8140) {  // spaces
        g.width = g.height = 0;
        g.origin_x = 0;
        g.origin_y = 0;
        g.gray.clear();
        return true;
    }
    g.width = adv > 2 ? adv - 2 : 1;
    g.height = h > 2 ? h - 2 : 1;
    g.origin_x = 1;
    g.origin_y = g.ascent - 1;
    g.gray.assign(size_t(g.width * g.height), 0);
    for (int y = 0; y < g.height; y++)
        for (int x = 0; x < g.width; x++)
            if (x == 0 || y == 0 || x == g.width - 1 || y == g.height - 1) g.gray[size_t(y * g.width + x)] = 64;
    return true;
}
}  // namespace rt
#endif

SHIM(gdi32, CreateFontA, 56) {
    Font f;
    f.height = int32_t(arg(c, 0));
    f.width = int32_t(arg(c, 1));
    f.weight = int32_t(arg(c, 4));
    f.charset = arg(c, 8);
    f.face = arg(c, 13) ? gstr(arg(c, 13)) : "";
    std::lock_guard<std::mutex> lock(font_mutex);
    uint32_t h = next_font++;
    fonts[h] = f;
    if (trace_imports) log("  CreateFontA height %d weight %d charset %u", f.height, f.weight, f.charset);
    return h;
}

// SelectObject also lands here for fonts (gdi32!SelectObject in shims_win.cpp handles the rest).
uint32_t rt_select_font(uint32_t h) {
    std::lock_guard<std::mutex> lock(font_mutex);
    uint32_t old = selected;
    if (fonts.count(h)) selected = h;
    return old ? old : 0x40000 + 13;  // SYSTEM_FONT
}

SHIM(gdi32, GetTextMetricsA, 8) {
    Font f = current_font();
    Glyph g;
    font_glyph({f.height, f.weight, f.face}, 0x82A0, g);  // a full-width kana for the metrics
    uint32_t t = arg(c, 1);
    memset(GM + t, 0, 56);
    st32(t, uint32_t(g.ascent + g.descent));  // tmHeight
    st32(t + 4, uint32_t(g.ascent));
    st32(t + 8, uint32_t(g.descent));
    st32(t + 20, uint32_t(g.advance / 2));  // tmAveCharWidth
    st32(t + 24, uint32_t(g.advance));      // tmMaxCharWidth
    st32(t + 28, uint32_t(f.weight));
    st8(t + 52, 0x31);                      // tmPitchAndFamily: fixed pitch, vector, truetype
    st8(t + 55, uint8_t(f.charset));
    return 1;
}

SHIM(gdi32, GetGlyphOutlineA, 28) {
    uint32_t code = arg(c, 1), format = arg(c, 2), gm = arg(c, 3), cap = arg(c, 4), buf = arg(c, 5);
    Font f = current_font();
    Glyph g;
    if (!font_glyph({f.height, f.weight, f.face}, code, g)) return 0xFFFFFFFF;
    // GLYPHMETRICS: black box w/h, origin x/y, cell increments.
    st32(gm, uint32_t(g.width ? g.width : 1));
    st32(gm + 4, uint32_t(g.height ? g.height : 1));
    st32(gm + 8, uint32_t(g.origin_x));
    st32(gm + 12, uint32_t(g.origin_y));
    st16(gm + 16, uint32_t(g.advance));
    st16(gm + 18, 0);
    uint32_t levels;
    switch (format) {
    case 4: levels = 4; break;    // GGO_GRAY2_BITMAP: 0..4
    case 5: levels = 16; break;   // GGO_GRAY4_BITMAP: 0..16
    case 6: levels = 64; break;   // GGO_GRAY8_BITMAP: 0..64
    case 1: levels = 1; break;    // GGO_BITMAP
    default: return 0;            // GGO_METRICS
    }
    uint32_t w = uint32_t(g.width), h = uint32_t(g.height);
    if (!w || !h) return 0;
    uint32_t pitch = levels == 1 ? ((w + 31) / 32) * 4 : (w + 3) & ~3u;
    uint32_t size = pitch * h;
    if (!cap || !buf) return size;
    memset(GM + buf, 0, std::min(cap, size));
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = g.gray[size_t(y * w + x)];  // 0..64
            if (levels == 1) {
                if (v >= 32 && y * pitch + x / 8 < cap) GM[buf + y * pitch + x / 8] |= uint8_t(0x80 >> (x & 7));
            } else if (y * pitch + x < cap) {
                GM[buf + y * pitch + x] = uint8_t((v * levels + 32) / 64);
            }
        }
    return size;
}
