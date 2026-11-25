// Metal Shading Language for the Metal renderer: the game's vs_1_x / ps_1_x shaders
// translated from their decoded form (shader.h), and fixed-function shaders that read the
// Direct3D pipeline state from a uniform block.
//
// Every vertex function writes `VOut` and every fragment function reads it, so any vertex
// function (a translated shader or the fixed-function one) pairs with any fragment one.
// Buffers: 0..15 vertex streams, 29 the defaults for missing inputs, 30 the vertex shader
// constants (or the fixed-function block), 31 the per-draw block. Fragment: textures and
// samplers 0..7, buffer 0 the pixel shader constants, buffer 1 the per-draw block.
#pragma once

#include <string>

#include "shader.h"

namespace rt {
namespace msl {

// Shared declarations: VOut, the uniform blocks, helpers. Compile it together with any of
// the functions below (they all expect it in front).
std::string prelude();

// Fixed-function vertex (`ff_vertex`) and fragment (`ff_fragment`) functions.
std::string fixed_function();

// A translated shader as one function named `name`. Inputs of a vertex shader are
// [[attribute(n)]] for input register vN. Returns "" (and sets `error`) if it cannot.
std::string vertex(const shader::Program &p, const std::string &name, std::string *error = nullptr);
std::string fragment(const shader::Program &p, const std::string &name, std::string *error = nullptr);

// Attribute slots of the fixed-function vertex input (per D3D vertex usage).
enum FFSlot {
    FF_POSITION = 0, FF_POSITIONT = 1, FF_BLENDWEIGHT = 2, FF_BLENDINDICES = 3, FF_NORMAL = 4, FF_COLOR0 = 5,
    FF_COLOR1 = 6, FF_TEXCOORD0 = 7, FF_PSIZE = 15, FF_SLOTS = 16,
};

}  // namespace msl
}  // namespace rt
