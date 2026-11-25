// Glyphs for the headless (non-Apple) build: a TrueType font through stb_truetype.
// RECOMP_FONT names the font file (a .ttc uses its first face); by default MS Gothic from
// the Windows fonts folder as WSL sees it. Without a font, glyphs are outlined boxes.
#if !defined(__APPLE__)
#include <iconv.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#include "font.h"
#include "runtime.h"

namespace rt {
namespace {
std::mutex font_mutex;
std::vector<uint8_t> font_data;
stbtt_fontinfo font;
bool font_ok = false, font_tried = false;

void load_font() {
    font_tried = true;
    const char *path = getenv("RECOMP_FONT");
    if (!path) path = "/mnt/c/Windows/Fonts/msgothic.ttc";
    FILE *f = fopen(path, "rb");
    if (!f) {
        log("recomp: no font at %s: text shows as boxes", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    font_data.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    size_t got = fread(font_data.data(), 1, font_data.size(), f);
    fclose(f);
    if (got != font_data.size()) return;
    int offset = stbtt_GetFontOffsetForIndex(font_data.data(), 0);
    font_ok = offset >= 0 && stbtt_InitFont(&font, font_data.data(), offset);
}

uint32_t to_unicode(uint32_t code) {
    if (code < 0x80) return code;
    char in[2];
    size_t n = 0;
    if (code > 0xFF) in[n++] = char(code >> 8);
    in[n++] = char(code & 0xFF);
    uint16_t out = 0;
    iconv_t cd = iconv_open("UTF-16LE", "CP932");
    if (cd == iconv_t(-1)) return '?';
    char *ip = in, *op = reinterpret_cast<char *>(&out);
    size_t il = n, ol = 2;
    iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    return out ? out : '?';
}

void box(int h, uint32_t code, Glyph &g) {
    bool wide = code > 0xFF;
    int adv = wide ? h : h / 2;
    g.advance = adv;
    g.ascent = h * 7 / 8;
    g.descent = h - g.ascent;
    g.width = adv > 2 ? adv - 2 : 1;
    g.height = h > 2 ? h - 2 : 1;
    g.origin_x = 1;
    g.origin_y = g.ascent - 1;
    g.gray.assign(size_t(g.width * g.height), 0);
    for (int y = 0; y < g.height; y++)
        for (int x = 0; x < g.width; x++)
            if (x == 0 || y == 0 || x == g.width - 1 || y == g.height - 1) g.gray[size_t(y * g.width + x)] = 64;
}
}  // namespace

bool font_glyph(const FontSpec &f, uint32_t code, Glyph &g) {
    std::lock_guard<std::mutex> lock(font_mutex);
    if (!font_tried) load_font();
    int h = f.height < 0 ? -f.height : f.height;
    if (!h) h = 16;
    if (!font_ok) {
        box(h, code, g);
        return true;
    }
    float scale = f.height < 0 ? stbtt_ScaleForMappingEmToPixels(&font, float(h)) : stbtt_ScaleForPixelHeight(&font, float(h));
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    g.ascent = int(std::lround(asc * scale));
    g.descent = int(std::lround(-desc * scale));
    uint32_t cp = to_unicode(code);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&font, int(cp), &adv, &lsb);
    g.advance = int(std::lround(adv * scale));
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font, int(cp), scale, scale, &x0, &y0, &x1, &y1);
    g.width = x1 - x0;
    g.height = y1 - y0;
    g.origin_x = x0;
    g.origin_y = -y0;
    g.gray.assign(size_t(std::max(0, g.width * g.height)), 0);
    if (g.width > 0 && g.height > 0) {
        std::vector<uint8_t> px(size_t(g.width * g.height));
        stbtt_MakeCodepointBitmap(&font, px.data(), g.width, g.height, g.width, scale, scale, int(cp));
        for (size_t i = 0; i < px.size(); i++) g.gray[i] = uint8_t((px[i] * 64 + 127) / 255);
        if (f.weight >= 600) {  // bold: thicken by one pixel
            std::vector<uint8_t> b = g.gray;
            for (int y = 0; y < g.height; y++)
                for (int x = 1; x < g.width; x++)
                    b[size_t(y * g.width + x)] = std::max(b[size_t(y * g.width + x)], g.gray[size_t(y * g.width + x - 1)]);
            g.gray.swap(b);
        }
    } else {
        g.width = g.height = 0;
    }
    return true;
}

}  // namespace rt
#endif
