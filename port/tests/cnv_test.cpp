// .cnv decoding (src/cnv.*) and the image C API, on synthetic data only.
// Usage: cnv_test <port/ios/Fixtures dir>. Exit status 0 = PASS.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/cnv.h"
#include "../src/daybreak_c.h"

using namespace db;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

// Header (bpp, width, height, pitch, packed) followed by `pixels` (BGRA, pitch-wide rows).
std::vector<uint8_t> image(uint8_t bpp, uint32_t w, uint32_t h, uint32_t pitch, uint32_t packed,
                           const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> v = {bpp};
    put32(v, w);
    put32(v, h);
    put32(v, pitch);
    put32(v, packed);
    v.insert(v.end(), pixels.begin(), pixels.end());
    return v;
}

std::vector<uint8_t> sound(uint32_t data_size, size_t actual) {
    std::vector<uint8_t> v = {1, 0, 1, 0};  // PCM, mono
    put32(v, 22050);
    put32(v, 44100);
    v.insert(v.end(), {2, 0, 16, 0, 0, 0});  // block align, bits, cbSize
    put32(v, data_size);
    for (size_t i = 0; i < actual; ++i) v.push_back(uint8_t(i));
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <Fixtures dir>\n", argv[0]);
        return 2;
    }
    std::string err;

    // 24 bpp, 3x2 with pitch 4: the fourth pixel of each row is padding (0xEE) and must be
    // dropped; alpha becomes 255 whatever the stored byte is.
    std::vector<uint8_t> px24;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t v = uint8_t(y * 16 + x);
            if (x == 3)
                px24.insert(px24.end(), {0xEE, 0xEE, 0xEE, 0xEE});
            else
                px24.insert(px24.end(), {uint8_t(v + 1), uint8_t(v + 2), uint8_t(v + 3), 0x00});  // B,G,R,X
        }
    Image img;
    check(cnv_kind(image(24, 3, 2, 4, 0, px24)) == CnvKind::Image, "24-bpp header classified as image");
    check(decode_image(image(24, 3, 2, 4, 0, px24), img, &err), "24-bpp padded image decodes");
    check(img.width == 3 && img.height == 2 && img.pitch == 4 && img.bpp == 24, "width/height/pitch from fields 1/2/3");
    std::vector<uint8_t> want24;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
            uint8_t v = uint8_t(y * 16 + x);
            want24.insert(want24.end(), {uint8_t(v + 3), uint8_t(v + 2), uint8_t(v + 1), 255});  // R,G,B,A
        }
    check(img.rgba == want24, "BGRA -> RGBA, padding dropped, opaque alpha");

    // 32 bpp keeps alpha; a wide non-square image must not be read as tall.
    std::vector<uint8_t> px32 = {10, 20, 30, 40, 50, 60, 70, 80};  // 2x1
    check(decode_image(image(32, 2, 1, 2, 0, px32), img, &err), "32-bpp 2x1 decodes");
    check(img.width == 2 && img.height == 1 && img.rgba == std::vector<uint8_t>({30, 20, 10, 40, 70, 60, 50, 80}),
          "32-bpp keeps alpha and row order");

    check(!decode_image(image(32, 2, 1, 2, 0, {1, 2, 3}), img, &err), "rejects short pixel data");
    check(!decode_image(image(32, 2, 1, 2, 0, std::vector<uint8_t>(12)), img, &err), "rejects extra pixel data");
    check(!decode_image(image(32, 3, 1, 2, 0, std::vector<uint8_t>(8)), img, &err), "rejects pitch < width");
    check(!decode_image(image(8, 2, 1, 2, 0, std::vector<uint8_t>(8)), img, &err), "rejects 8 bpp");
    check(!decode_image(image(32, 2, 1, 2, 5, std::vector<uint8_t>(8)), img, &err), "rejects packed data");
    check(!decode_image(image(32, 0, 1, 0, 0, {}), img, &err), "rejects zero size");
    check(!decode_image({32, 1, 0}, img, &err), "rejects a truncated header");

    // Size limits: 16384 is the largest accepted dimension (cnv.cpp keeps the size math in
    // 64 bits below it); 16385 is rejected before any pixel buffer is allocated.
    check(decode_image(image(32, 16384, 1, 16384, 0, std::vector<uint8_t>(16384 * 4)), img, &err) &&
              img.rgba.size() == 16384 * 4,
          "16384x1 is accepted");
    check(!decode_image(image(32, 16385, 1, 16385, 0, std::vector<uint8_t>(16385 * 4)), img, &err),
          "16385 wide is rejected");
    check(!decode_image(image(32, 1, 16385, 1, 0, {}), img, &err), "16385 tall is rejected without pixel data");
    check(!decode_image(image(32, 1, 1, 0xFFFFFFFFu, 0, {}), img, &err), "pitch 0xFFFFFFFF is rejected");

    Sound s;
    check(cnv_kind(sound(4, 4)) == CnvKind::Sound, "PCM header classified as sound");
    check(decode_sound(sound(4, 6), s, &err) && s.channels == 1 && s.rate == 22050 && s.bits == 16 &&
              s.pcm == std::vector<uint8_t>({0, 1, 2, 3}),
          "sound fields and data_size-limited PCM");
    check(!decode_sound(sound(10, 4), s, &err), "rejects data_size past the end");
    check(cnv_kind({7, 7, 7}) == CnvKind::Unknown, "unknown header");
    check(cnv_name("data/stage01/a.bmp") == "data/stage01/a.cnv", "bmp name mapped to cnv");

    // C API on the committed fixtures (port/tools/make_fixture.py).
    const std::string base = std::string(argv[1]) + "/fixture.dat";
    const std::string overlay = std::string(argv[1]) + "/fixture-overlay.dat";
    const char* paths[] = {overlay.c_str(), base.c_str()};
    char cerr[256];
    DBArchives* a = db_archives_open(paths, 2, cerr, sizeof cerr);
    check(a != nullptr, "db_archives_open on overlay + base fixtures");
    const char* tex = "data/stage01/\x83\x65\x83\x58\x83\x67\x91\x90" "01.bmp";  // テスト草01.bmp
    DBImage* im = db_image_load(a, tex, int32_t(strlen(tex)), cerr, sizeof cerr);
    check(im && db_image_width(im) == 2 && db_image_height(im) == 2 && db_image_bpp(im) == 32,
          "db_image_load maps .bmp to .cnv and falls through to the base archive");
    const uint8_t want[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    check(im && memcmp(db_image_rgba(im), want, 16) == 0, "fixture pixels: red, green, blue, white");
    db_image_free(im);
    const char* missing = "data/stage01/none.bmp";
    check(!db_image_load(a, missing, int32_t(strlen(missing)), cerr, sizeof cerr) &&
              strstr(cerr, "missing") != nullptr,
          "missing texture reports an error");
    const char* mesh = "data/stage01/00.x";
    check(!db_image_load(a, mesh, int32_t(strlen(mesh)), cerr, sizeof cerr), "non-.cnv entry is not loaded as an image");
    db_archives_free(a);
    check(!db_archives_open(paths, 0, cerr, sizeof cerr), "db_archives_open rejects an empty list");

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
