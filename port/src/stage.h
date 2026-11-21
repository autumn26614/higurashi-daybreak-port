// Stage analysis shared by the stageinfo CLI and the iOS test app (via daybreak_c.h).
// Follows the game's Stage::Load (0x405B50); see analysis/05-stage-loading.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "archive.h"
#include "xfile.h"

namespace db {

// cp932 -> UTF-8 via iconv when available; returns the input unchanged otherwise.
std::string utf8(const std::string& cp932);

struct Box {
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    bool empty() const { return lo[0] > hi[0]; }
    void add(const double* p);
    void add(const Box& b);
};

struct MaterialInfo {
    std::string texture;  // raw cp932 TextureFilename, may be empty
    std::string effect;   // EffectInstance filename, if any
};

struct Stats {
    int frames = 0, meshes = 0, nonidentity_frames = 0, anim_sets = 0, skinned = 0;
    long verts = 0, faces = 0, tris = 0;
    int materials = 0, mesh_without_normals = 0, mesh_without_uv = 0, with_vertex_colors = 0;
    std::vector<MaterialInfo> mats;  // all material slots, in order
    Box local, world;
    std::vector<std::string> tree;
};

// Walks one parsed .x file (frames, meshes, materials).
Stats analyze_x(const XFile& x, bool want_tree);

// Reads and parses one .x entry from the archive. err = "missing" if absent.
bool load_x(const Source& a, const std::string& name, XFile& x, std::string* err);

// Texture resolution as in the engine: _splitpath dir of the .x (0x5D6C20) + name
// (0x5CD530), Image::Load swaps the last 3 chars for "cnv" (0x5E5D80), and the
// texture loader probes "<name minus 4 chars>_N.bmp" mip levels (0x5D8830).
struct TexStatus {
    std::string archive_name;
    bool found = false;
    int mips = 0;
};
TexStatus resolve_texture(const Source& a, const std::string& dir, const std::string& tex);

struct StageTexture {
    std::string name;  // raw cp932 name as written in the .x
    TexStatus status;
};

// Whole-stage summary: totals over slots 00-07 for the plain ("%02d.x", index 0) and
// "b" ("%02db.x", index 1) sets, plus every texture referenced by either set,
// sorted by raw bytes (same set and order as the stageinfo CLI listing).
struct StageSummary {
    int stage = 0;
    std::string dir;
    int meshes[2] = {0, 0};
    long tris[2] = {0, 0};
    std::vector<std::string> errors;  // per-file parse errors (missing files are not errors)
    std::vector<StageTexture> textures;
};
StageSummary summarize_stage(const Source& a, int stage);

}  // namespace db
