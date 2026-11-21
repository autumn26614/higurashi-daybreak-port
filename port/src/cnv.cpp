#include "cnv.h"

#include <cstring>

namespace db {

namespace {
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
const size_t kImageHeader = 17;
const size_t kSoundHeader = 22;
}  // namespace

CnvKind cnv_kind(const std::vector<uint8_t>& data) {
    if (data.size() >= kSoundHeader && u16(data.data()) == 1) return CnvKind::Sound;
    if (data.size() >= kImageHeader && (data[0] == 24 || data[0] == 32)) return CnvKind::Image;
    return CnvKind::Unknown;
}

bool decode_image(const std::vector<uint8_t>& data, Image& out, std::string* err) {
    if (data.size() < kImageHeader) {
        *err = "image .cnv shorter than its 17-byte header";
        return false;
    }
    const uint8_t* p = data.data();
    Image img;
    img.bpp = p[0];
    img.width = u32(p + 1);
    img.height = u32(p + 5);
    img.pitch = u32(p + 9);
    uint32_t packed = u32(p + 13);
    if (img.bpp != 24 && img.bpp != 32) {
        *err = "unsupported image .cnv bpp " + std::to_string(img.bpp);
        return false;
    }
    if (packed != 0) {
        *err = "packed image .cnv (never seen in the shipped data)";
        return false;
    }
    // Real textures are at most 1024 wide; this bound keeps the size math in 64 bits.
    if (img.width == 0 || img.height == 0 || img.width > 16384 || img.height > 16384 || img.pitch > 16384 ||
        img.pitch < img.width) {
        *err = "implausible image .cnv size " + std::to_string(img.width) + "x" + std::to_string(img.height) +
               " pitch " + std::to_string(img.pitch);
        return false;
    }
    const uint64_t stored = uint64_t(img.pitch) * img.height * 4;
    if (data.size() != kImageHeader + stored) {
        *err = "image .cnv payload is " + std::to_string(data.size()) + " bytes, header implies " +
               std::to_string(kImageHeader + stored);
        return false;
    }
    img.rgba.resize(size_t(img.width) * img.height * 4);
    uint8_t* d = img.rgba.data();
    for (uint32_t y = 0; y < img.height; ++y) {
        const uint8_t* s = p + kImageHeader + size_t(y) * img.pitch * 4;
        for (uint32_t x = 0; x < img.width; ++x, s += 4, d += 4) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = img.bpp == 32 ? s[3] : 255;
        }
    }
    out = std::move(img);
    return true;
}

bool decode_sound(const std::vector<uint8_t>& data, Sound& out, std::string* err) {
    if (data.size() < kSoundHeader || u16(data.data()) != 1) {
        *err = "not a PCM sound .cnv";
        return false;
    }
    const uint8_t* p = data.data();
    Sound s;
    s.channels = u16(p + 2);
    s.rate = u32(p + 4);
    s.bits = u16(p + 14);
    uint32_t size = u32(p + 18);
    if (size > data.size() - kSoundHeader) {
        *err = "sound .cnv data size " + std::to_string(size) + " exceeds the payload";
        return false;
    }
    s.pcm.assign(p + kSoundHeader, p + kSoundHeader + size);
    out = std::move(s);
    return true;
}

std::string cnv_name(const std::string& name) {
    std::string n = name;
    if (n.size() >= 3) n.replace(n.size() - 3, 3, "cnv");
    return n;
}

}  // namespace db
