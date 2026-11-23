// Renderable stage geometry: what Stage::Load (0x405B50) hands to D3D, flattened into
// one draw batch per (slot, mesh, material subset). See analysis/05-stage-loading.md.
//
// Coordinates stay as stored (Direct3D, left-handed, y up, vertices already in world
// units; stage .x files have no frames). Faces are fan-triangulated as D3DX does, with
// the original winding: D3D culls counter-clockwise (CULLMODE=3), so front faces are
// clockwise on screen. A renderer with the same clip space (Metal) needs no flip.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "archive.h"
#include "stage.h"

namespace db {

struct GMaterial {
    float diffuse[4] = {1, 1, 1, 1};  // .x Material: faceColor RGBA
    float power = 0;                  // non-zero: the game draws the subset additively (0x5D1750)
    float specular[3] = {0, 0, 0};
    float emissive[3] = {0, 0, 0};
    std::string texture;              // raw cp932 TextureFilename, may be empty
    TexStatus tex;                    // resolved archive entry
};

struct GBatch {
    int slot = 0;                     // 0-7: which %02d.x file
    int mesh = 0;                     // mesh index within the slot file
    int material = 0;                 // material index within the mesh
    std::vector<float> positions;     // x, y, z per vertex
    std::vector<float> uvs;           // u, v per vertex (0, 0 if the mesh has none)
    std::vector<uint8_t> colors;      // R, G, B, A per vertex (255s if the mesh has none)
    bool has_colors = false;          // the mesh carries MeshVertexColors
    std::vector<uint32_t> indices;    // triangles, original winding
    std::vector<float> normals;       // x, y, z per vertex (characters only: outline pass)
    GMaterial mat;
};

// env.dat: 21 little-endian int32 (Stage::Load step 1).
struct StageEnv {
    bool present = false;
    int32_t fog_start = 0, fog_end = 0;  // ints 0, 1 (inferred fog start/end)
    uint8_t fog_rgb[3] = {0, 0, 0};      // ints 2-4, low byte each
    int32_t params[4] = {0, 0, 0, 0};    // ints 5-8, meaning unknown
    float spawn[4][3] = {};              // ints 9-20: players 1-4
};

struct StageGeometry {
    int stage = 0;
    int variant = 0;  // 0 = "%02d.x" (game default), 1 = "%02db.x"
    bool mode3 = false;  // passes marked mode3 use render mode 3 colours (plain set only)
    std::string dir;
    StageEnv env;
    std::vector<GBatch> batches;
    std::vector<std::string> errors;  // per-file parse problems (a missing slot is not one)
    Box bounds;                       // over every batch except the sky (slot 0)
    size_t triangles() const;
};

bool parse_env(const std::vector<uint8_t>& d, StageEnv& env);

// Builds the batches of one parsed .x file for `slot` (used by load_stage_geometry,
// exposed for tests). `dir` is the texture search directory with a trailing '/'.
void append_x_batches(const Source& a, const XFile& x, const std::string& dir, int slot, StageGeometry& g);

StageGeometry load_stage_geometry(const Source& a, int stage, int variant);

// Slot for characters placed on a stage (drawn after the stage's own slots, recipe.h).
constexpr int kCharacterSlot = 8;

// Appends a posed character's batches to a stage, turned by `yaw` radians about +y
// (a model faces -z) and moved to `pos`; they go into kCharacterSlot. Normals turn too.
void place_character(StageGeometry& stage, const StageGeometry& character, const float pos[3], float yaw);

// Yaw that turns a model (facing -z) to look from `from` towards `to` (in x/z).
float facing_yaw(const float from[3], const float to[3]);

}  // namespace db
