// Glyphs on Apple platforms (GetGlyphOutlineA): CoreText with Hiragino Sans, the system
// Japanese gothic (the game asks for MS Gothic).
#if defined(__APPLE__)
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <cmath>
#include <map>
#include <mutex>

#include "font.h"

namespace rt {
namespace {
std::mutex font_mutex;
std::map<std::pair<int, bool>, CTFontRef> fonts;  // (em size, bold)

CTFontRef font_for(int em, bool bold) {
    auto key = std::make_pair(em, bold);
    auto it = fonts.find(key);
    if (it != fonts.end()) return it->second;
    CFStringRef name = bold ? CFSTR("HiraginoSans-W6") : CFSTR("HiraginoSans-W3");
    CTFontRef f = CTFontCreateWithName(name, CGFloat(em), nullptr);
    fonts[key] = f;
    return f;
}

UniChar to_unicode(uint32_t code) {
    if (code < 0x80) return UniChar(code);
    uint8_t in[2];
    CFIndex n = 0;
    if (code > 0xFF) in[n++] = uint8_t(code >> 8);
    in[n++] = uint8_t(code & 0xFF);
    CFStringRef s = CFStringCreateWithBytes(nullptr, in, n, kCFStringEncodingDOSJapanese, false);
    UniChar u = '?';
    if (s) {
        if (CFStringGetLength(s) > 0) u = CFStringGetCharacterAtIndex(s, 0);
        CFRelease(s);
    }
    return u;
}
}  // namespace

bool font_glyph(const FontSpec &f, uint32_t code, Glyph &g) {
    std::lock_guard<std::mutex> lock(font_mutex);
    int h = f.height < 0 ? -f.height : f.height;
    if (!h) h = 16;
    bool bold = f.weight >= 600;
    CTFontRef probe = font_for(h, bold);
    int em = h;
    if (f.height > 0) {  // cell height: ascent + descent
        CGFloat cell = CTFontGetAscent(probe) + CTFontGetDescent(probe);
        em = std::max(1, int(std::lround(h * CGFloat(h) / std::max<CGFloat>(1, cell))));
    }
    CTFontRef font = font_for(em, bold);
    g.ascent = int(std::lround(CTFontGetAscent(font)));
    g.descent = int(std::lround(CTFontGetDescent(font)));
    UniChar u = to_unicode(code);
    CGGlyph glyph = 0;
    if (!CTFontGetGlyphsForCharacters(font, &u, &glyph, 1)) glyph = 0;
    CGSize adv;
    CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph, &adv, 1);
    g.advance = int(std::lround(adv.width));
    CGRect box = CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
    if (box.size.width <= 0 || box.size.height <= 0) {
        g.width = g.height = 0;
        g.origin_x = g.origin_y = 0;
        g.gray.clear();
        return true;
    }
    int x0 = int(std::floor(box.origin.x)), y0 = int(std::floor(box.origin.y));
    int x1 = int(std::ceil(box.origin.x + box.size.width)), y1 = int(std::ceil(box.origin.y + box.size.height));
    g.width = x1 - x0;
    g.height = y1 - y0;
    g.origin_x = x0;
    g.origin_y = y1;  // top of the black box above the baseline
    std::vector<uint8_t> px(size_t(g.width * g.height), 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), size_t(g.width), size_t(g.height), 8, size_t(g.width), gray,
                                             kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (ctx) {
        CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        CGContextSetShouldAntialias(ctx, true);
        CGPoint pos = CGPointMake(-x0, -y0);  // CoreGraphics' origin is bottom-left
        CTFontDrawGlyphs(font, &glyph, &pos, 1, ctx);
        CGContextRelease(ctx);
    }
    g.gray.resize(px.size());
    // A bitmap context's memory starts with the top row, as GetGlyphOutline wants.
    for (int y = 0; y < g.height; y++)
        for (int x = 0; x < g.width; x++)
            g.gray[size_t(y * g.width + x)] = uint8_t((px[size_t(y * g.width + x)] * 64 + 127) / 255);
    return true;
}

}  // namespace rt
#endif
