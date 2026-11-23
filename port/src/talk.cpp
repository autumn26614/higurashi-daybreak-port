#include "talk.h"

#include <algorithm>

#include "scenario.h"

namespace db {
namespace talk {

namespace {

int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;  // the game's hex reader (0x5E2300) has no error path; treat junk as 0
}

}  // namespace

Layout layout(const std::string& all, const Params& p) {
    Layout l;
    const std::string s = all.substr(0, all.find('\0'));  // strlen
    l.text_bytes = s.size();
    uint8_t rgb[3] = {p.rgb[0], p.rgb[1], p.rgb[2]};
    int x = 1, y = 1, lines_left = p.max_lines;
    const int line_h = p.height + p.line_spacing;
    // Wrap to the next line; false when out of lines (or, for a wrap, out of height).
    auto next_line = [&](bool check_height) {
        x = 1;
        if (check_height && p.bitmap_height - y < line_h) return false;
        y += line_h;
        if (--lines_left <= 0) return false;
        l.lines++;
        return true;
    };
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t c = uint8_t(s[i]);
        const int bytes = scn::is_lead_byte(c) && i + 1 < s.size() ? 2 : 1;
        const uint16_t code = bytes == 2 ? uint16_t(c << 8 | uint8_t(s[i + 1])) : c;
        if (code == '\\' && i + 1 < s.size()) {
            const uint8_t k = uint8_t(s[i + 1]);
            if (scn::is_lead_byte(k)) {  // "\" + a two-byte character: skipped
                i += 1;
                continue;
            }
            if (k == 'c' && i + 8 <= s.size()) {
                for (int ch = 0; ch < 3; ch++)
                    rgb[ch] = uint8_t(hexval(uint8_t(s[i + 2 + ch * 2])) << 4 | hexval(uint8_t(s[i + 3 + ch * 2])));
                i += 8;
            } else if (k == 'e') {
                std::copy(p.rgb, p.rgb + 3, rgb);
                i += 2;
            } else if (k == 'n') {
                i += 2;
                if (!next_line(false)) {
                    l.stopped = true;
                    break;
                }
            } else {
                i += 2;
            }
            continue;
        }
        const int adv = (p.height / 2) * bytes + p.extra_advance;
        if (p.wrap_width - x < adv && !next_line(true)) {
            l.stopped = true;
            break;
        }
        if (code != 0x20 && code != 0x8140 && x + adv < p.bitmap_width && y + p.height < p.bitmap_height) {
            Glyph g;
            g.code = code;
            g.bytes = uint8_t(bytes);
            g.x = x;
            g.y = y;
            std::copy(rgb, rgb + 3, g.rgb);
            l.glyphs.push_back(g);
        }
        x += adv;
        i += size_t(bytes);
    }
    return l;
}

void glyph_pixel(int g, int cell_y, const uint8_t rgb[3], const Params& p, uint8_t out[4]) {
    if (g <= 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    out[3] = uint8_t((14 * std::min(g, 16)) | 3);
    if (!p.gradient) {
        out[0] = rgb[0], out[1] = rgb[0], out[2] = rgb[2];  // the game reads R twice
        return;
    }
    for (int ch = 0; ch < 3; ch++) out[ch] = uint8_t(255 - (((cell_y * (255 - rgb[ch])) / p.height) & 0xFF));
}

std::vector<uint8_t> draw_text(const Layout& l, const std::map<uint16_t, Mask>& masks, const Params& p) {
    const int W = p.bitmap_width, H = p.bitmap_height;
    std::vector<uint8_t> bmp(size_t(W) * size_t(H) * 4, 0);
    for (const Glyph& g : l.glyphs) {
        auto it = masks.find(g.code);
        if (it == masks.end()) continue;
        const Mask& m = it->second;
        for (int row = 0; row < m.height; row++)
            for (int col = 0; col < m.width; col++) {
                const int cov = m.coverage[size_t(row * m.width + col)];
                if (cov == 0) continue;
                const int px = g.x + m.left + col, py = g.y + m.top + row;
                if (px < 0 || py < 0 || px >= W || py >= H) continue;
                glyph_pixel(cov, m.top + row, g.rgb, p, &bmp[size_t(py * W + px) * 4]);
            }
    }
    return bmp;
}

namespace {

// Alpha-blends a modulated sprite region (SRCALPHA, INVSRCALPHA; colour = texel * mod).
void blit(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src, int src_w, int src_h, int sx, int sy, int w,
          int h, int dx, int dy, const float mod[4]) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int tx = sx + x, ty = sy + y, ox = dx + x, oy = dy + y;
            if (tx < 0 || ty < 0 || tx >= src_w || ty >= src_h) continue;
            if (ox < 0 || oy < 0 || ox >= Frame::kWidth || oy >= Frame::kHeight) continue;
            const uint8_t* s = &src[size_t(ty * src_w + tx) * 4];
            uint8_t* d = &dst[size_t(oy * Frame::kWidth + ox) * 4];
            const float a = s[3] / 255.0f * mod[3];
            for (int c = 0; c < 3; c++) {
                const float v = s[c] / 255.0f * mod[c] * a + d[c] / 255.0f * (1 - a);
                d[c] = uint8_t(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f)));
            }
        }
}

}  // namespace

std::vector<uint8_t> compose(const std::vector<uint8_t>& background, const std::vector<uint8_t>& window,
                             int window_w, int window_h, const std::vector<uint8_t>& text, const Params& p,
                             size_t reveal, bool arrow) {
    const size_t n = size_t(Frame::kWidth) * size_t(Frame::kHeight) * 4;
    std::vector<uint8_t> out = background.size() == n ? background : std::vector<uint8_t>(n, 0);
    for (size_t i = 3; i < n; i += 4) out[i] = 255;
    const float half[4] = {1, 1, 1, 128 / 255.0f}, white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1};
    if (window.size() == size_t(window_w) * size_t(window_h) * 4) {
        blit(out, window, window_w, window_h, 0, 0, 780, 150, 10, 440, half);
        if (arrow) blit(out, window, window_w, window_h, 0, 160, 64, 32, 720, 550, white);
    }
    const int cols = 32, cells = 128, cw = 24, chh = p.height + p.line_spacing;
    const size_t shown = std::min(reveal, size_t(cells));
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < shown; i++) {
            const int c = int(i) % cols, r = int(i) / cols;
            const int ox = pass == 0 ? 23 : 20, oy = pass == 0 ? 453 : 450;
            blit(out, text, p.bitmap_width, p.bitmap_height, c * cw, r * chh, cw, chh, ox + c * cw, oy + r * chh,
                 pass == 0 ? black : white);
        }
    return out;
}

}  // namespace talk
}  // namespace db
