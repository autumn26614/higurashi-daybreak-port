// The talk scene's text box (scene 8, ctor 0x413DB0), as daybreak.exe draws it.
//
// Text object (ctor 0x5E15A0, font 0x5E1690, layout 0x5E16E0), talk-scene arguments:
//   font "ＭＳ ゴシック" (0x62B850), height 24, bold, SHIFTJIS, fixed pitch (CreateFontA 0x5E1761)
//   default colour 255,255,255 (this+0x20..0x22), gradient on (this+0x130 = 1 from the ctor;
//   only the scene at 0x5C5673 turns it off)
//   wrap width 760 (+0x140), max lines 5 (+0x144), extra advance 0 (+0x13C), line spacing 10 (+0x138)
//
// DrawText 0x5E1980 lays a cp932 C string out into a bitmap:
//   start x = 1, y = 1; stops at the first NUL (strlen) or when lines / height run out
//   a character takes 1 or 2 bytes (lead test 0x5E22B0); its advance is
//     (height / 2) * bytes + extra           -- a fixed 12 / 24 px cell, no font metrics
//   before a character: if (wrap width - x) < advance, wrap: x = 1, y += height + spacing
//   space (0x20) and ideographic space (0x8140) only advance
//   \n wraps the same way; \cRRGGBB sets the colour; \e resets it; other \x are skipped
//   a glyph is drawn only if it fits the bitmap (x + advance < width, y + height < height)
// Glyph pixels (0x5E1850, GetGlyphOutlineA GGO_GRAY4_BITMAP: coverage g = 0..16):
//   g == 0 leaves the pixel; else alpha = (14 * g) | 3   (0xE000000 * g | 0x3000000)
//   gradient: channel = 255 - (cell_y * (255 - c)) / height, cell_y = row from the cell top,
//   i.e. white at the top of the line fading to the colour at the bottom
//   flat (gradient off): R = c.r, G = c.r, B = c.b  (the game reads R twice)
//
// The scene (draw at 0x4148AC) on its 800x600 screen:
//   window: data/talk/window.bmp region (0,0,780,150) at (10,440), modulated 0x80FFFFFF (50%)
//   text bitmap, cut into 24x34 cells (32 per row, cells 0..127 in reading order), each cell
//   drawn once in black at (23 + 24c, 453 + 34r) as a shadow and once in white at
//   (20 + 24c, 450 + 34r); only cells below the reveal counter are drawn
//   reveal counter: +1 per frame after a 30-frame delay, up to strlen(text); a button press
//   jumps to the end
//   next-page arrow: window.bmp region (0,160,64,32) at (720,550) once the counter reached the
//   end, shown for 30 of every 60 frames
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace db {
namespace talk {

struct Params {
    int height = 24;
    int line_spacing = 10;
    int wrap_width = 760;
    int max_lines = 5;
    int extra_advance = 0;
    int bitmap_width = 768;   // 32 cells of 24 px
    int bitmap_height = 170;  // 5 lines of 34 px
    uint8_t rgb[3] = {255, 255, 255};
    bool gradient = true;
};

struct Glyph {
    uint16_t code = 0;  // cp932: one byte, or lead << 8 | trail
    uint8_t bytes = 1;
    int x = 0, y = 0;   // cell origin in the text bitmap
    uint8_t rgb[3] = {255, 255, 255};
};

struct Layout {
    std::vector<Glyph> glyphs;  // drawn glyphs only (not spaces)
    int lines = 1;
    bool stopped = false;       // ran out of lines or bitmap height
    size_t text_bytes = 0;      // strlen: the reveal counter's end
};

Layout layout(const std::string& text, const Params& p = Params());

// A glyph's coverage mask as GetGlyphOutline returns it for GGO_GRAY4: values 0..16,
// `left` / `top` = where its top-left pixel goes relative to the cell origin
// (gmptGlyphOrigin.x, ascent - gmptGlyphOrigin.y).
struct Mask {
    int width = 0, height = 0, left = 0, top = 0;
    std::vector<uint8_t> coverage;
};

// One RGBA pixel of glyph colour (straight alpha), or alpha 0 for coverage 0.
void glyph_pixel(int coverage, int cell_y, const uint8_t rgb[3], const Params& p, uint8_t out[4]);

// Draws the laid-out glyphs into a bitmap_width x bitmap_height RGBA bitmap (cleared to
// transparent). Glyphs without a mask are left blank.
std::vector<uint8_t> draw_text(const Layout& l, const std::map<uint16_t, Mask>& masks, const Params& p = Params());

// The talk scene frame on its 800x600 screen. `background` (800x600 RGBA) may be empty
// (black). `window` is data/talk/window.cnv decoded (RGBA); `reveal` the counter.
struct Frame {
    static constexpr int kWidth = 800, kHeight = 600;
};
std::vector<uint8_t> compose(const std::vector<uint8_t>& background, const std::vector<uint8_t>& window,
                             int window_w, int window_h, const std::vector<uint8_t>& text_bitmap, const Params& p,
                             size_t reveal, bool arrow);

// Background for a script, 800x600 RGBA (black where nothing is drawn). Win quotes
// (data/scenario/win/AA-BB-k.txt) show the two characters' portraits data/win/AAa and
// BBa (512x512 with alpha) at (-32,60) and (352,60), as the win screen draws its
// sprites at 0x5B78E5 / 0x5B78FD. Other scripts: black (the story scenes' portrait
// choice, data/talk/cNN-M, was not traced). `load_rgba` decodes an image entry.
std::vector<uint8_t> background(const std::string& script_name,
                                bool (*load_rgba)(void* ctx, const std::string& name, std::vector<uint8_t>& rgba,
                                                  int& w, int& h),
                                void* ctx);

}  // namespace talk
}  // namespace db
