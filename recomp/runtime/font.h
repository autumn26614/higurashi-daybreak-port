// Glyph rasterisation for GetGlyphOutlineA.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rt {

struct FontSpec {
    int32_t height;  // as given to CreateFont: negative is the em height, positive the cell height
    int32_t weight;
    std::string face;
};

struct Glyph {
    int width = 0, height = 0;   // black box
    int origin_x = 0, origin_y = 0;  // black box top-left relative to the pen on the baseline (y up)
    int advance = 0;
    int ascent = 0, descent = 0;
    std::vector<uint8_t> gray;   // width*height coverage, 0..64
};

// `code` is a code page 932 (Shift-JIS) character as GetGlyphOutlineA takes it.
bool font_glyph(const FontSpec &f, uint32_t code, Glyph &g);

}  // namespace rt
