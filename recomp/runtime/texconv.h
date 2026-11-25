// Direct3D texture formats to RGBA (both renderers).
#pragma once

#include <cstdint>
#include <vector>

#include "d3d9.h"
#include "shader.h"

namespace rt {
namespace d3d {

// Every pixel of a texture level or surface as RGBA floats (DXT decoded).
void decode_rgba(const Image &im, std::vector<shader::Vec4> &px);
// The same as 0xAARRGGBB words (Metal's BGRA8 in memory).
void decode_bgra8(const Image &im, std::vector<uint32_t> &px);

}  // namespace d3d
}  // namespace rt
