// The Metal renderer (Apple platforms): draws the game's Direct3D 9 calls with Metal and
// presents the 800x600 frame, scaled to fit, on a CAMetalLayer (with the movie on top).
#pragma once

#include <memory>

#include "d3d9.h"

namespace rt {
namespace d3d {

// `layer` is a CAMetalLayer* (as void* so C++ files can include this header). Its device is
// used; set its drawableSize as the view's size changes.
std::unique_ptr<Renderer> make_metal_renderer(void *layer);

}  // namespace d3d
}  // namespace rt
