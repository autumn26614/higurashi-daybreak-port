// CPU reference renderer for a stage: the same batches (geometry.h), draw recipe
// (recipe.h) and camera (camera.h) as the Metal renderer in the iOS app. Used by the
// render_stage tool, by the app when no Metal device exists, and by the app's tests to
// check the Metal output against it.
//
// Per pixel, as the D3D9 fixed-function state at 0x5D6500 / 0x406440:
//   colour = texture(uv) * vertex colour (MODULATE, colour and alpha)
//   fog    = linear in view-space depth between env start/end, towards the env colour
//   blend  = per pass (alpha / multiply / additive), Z test LESSEQUAL, Z write per pass
//   cull   = counter-clockwise on screen (front faces clockwise), wrap addressing
// The frame is cleared to the fog colour. Textures use the base level only.
#pragma once
#include <cstdint>
#include <vector>

#include "camera.h"
#include "geometry.h"

namespace db {

// Writes width*height*4 RGBA bytes (alpha 255) into `rgba`. Returns the number of
// pixels written by any pass (0 means nothing was visible).
long render_reference(const Source& a, const StageGeometry& g, const Camera& cam, int width, int height,
                      std::vector<uint8_t>& rgba);

}  // namespace db
