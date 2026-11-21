// .cnv payloads (already decrypted from the archive). The game asks for ".bmp"/".wav"
// names and swaps the last three characters for "cnv" (0x5E5D80).
// Format: analysis/04-scenes-and-cnv.md.
//
// Image (loader 0x5EA140):
//   u8  bpp            24 or 32 in the shipped data
//   u32 width          visible width (the texture loader 0x5D8830 sizes from width/height)
//   u32 height
//   u32 pitch          pixels per stored row, >= width (e.g. 257 wide, 260 pitch)
//   u32 packed_size    0 = raw pixels follow (non-zero never occurs in the shipped data)
//   pixels             height*pitch*4 bytes, BGRA, top-down (24-bpp is also 4 bytes/pixel)
// Field order checked against every image in daybreak00/04: 800x600 backgrounds are
// stored as (800, 600, 800). analysis/04-scenes-and-cnv.md had width and height mixed up.
// Sound: WAVEFORMATEX (18 bytes, cbSize 0) | u32 data_size | PCM bytes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace db {

enum class CnvKind { Image, Sound, Unknown };

// Classifies a payload by its first bytes (a sound starts with wFormatTag 1 = PCM).
CnvKind cnv_kind(const std::vector<uint8_t>& data);

struct Image {
    uint8_t bpp = 0;
    uint32_t width = 0;          // header field 1
    uint32_t height = 0;         // header field 2
    uint32_t pitch = 0;          // header field 3, as stored (padding is dropped from rgba)
    std::vector<uint8_t> rgba;   // width*height*4, R,G,B,A, top-down; A = 255 for 24-bpp
};

// Decodes a raw (unpacked) image .cnv with 24 or 32 bpp. Rejects anything else, and
// any payload whose size is not exactly 17 + height*pitch*4, or pitch < width.
bool decode_image(const std::vector<uint8_t>& data, Image& out, std::string* err);

struct Sound {
    uint16_t channels = 0, bits = 0;
    uint32_t rate = 0;
    std::vector<uint8_t> pcm;
};

// Decodes a PCM sound .cnv.
bool decode_sound(const std::vector<uint8_t>& data, Sound& out, std::string* err);

// Engine-style name mapping: "data/x/foo.bmp" -> "data/x/foo.cnv" (last three bytes).
std::string cnv_name(const std::string& bmp_or_wav);

}  // namespace db
