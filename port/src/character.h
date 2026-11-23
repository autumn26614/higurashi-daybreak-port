// Characters: data/<name>/00.X (and 01.X), skinned meshes on a 3ds Max Biped skeleton
// with ~120 AnimationSets of matrix keys (type 4), one key per 80 ticks = one per frame at
// 60 fps (D3DX default 4800 ticks per second; the files set no AnimTicksPerSecond).
// Loaded by XMesh::Load 0x5CC1B0 (D3DXLoadMeshHierarchyFromXInMemory, animation controller
// cloned, "Bip" frames looked up at 0x5CC630, skinning set up at 0x5CC780).
//
// Posing follows D3DX: a frame's matrix is its animation key (else FrameTransformMatrix),
// combined = local * parent (row vectors); a skinned vertex is
//   sum over bones of weight * (v * offset * combined)
// and a mesh without skin uses its frame's combined matrix.
//
// Shading is an APPROXIMATION. The game draws characters with compiled D3D9 shaders
// (embedded "pshader" blobs, not decoded). Here each vertex is lit like the fixed-function
// pipeline with the device light from 0x5D6500 (directional, direction normalize(1,-1,1),
// diffuse white): colour = clamp(emissive + diffuse * max(0, N . -L)), baked into the
// vertex colours so the stage renderers (raster.h, Metal) draw it unchanged.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "archive.h"
#include "geometry.h"

namespace db {

struct CharNode {
    std::string name;
    int parent = -1;
    std::array<double, 16> bind{};  // FrameTransformMatrix
};

struct CharBoneWeights {
    int node = -1;  // bone frame
    std::vector<uint32_t> vertices;
    std::vector<float> weights;
    std::array<double, 16> offset{};
};

struct CharMesh {
    int node = -1;                    // owning frame
    std::vector<float> positions;     // bind pose, xyz
    std::vector<float> normals;       // per vertex, xyz (averaged MeshNormals)
    std::vector<float> uvs;
    std::vector<uint32_t> tri_vertices;  // triangles (fan of each face), original winding
    std::vector<uint32_t> tri_material;  // material per triangle
    std::vector<GMaterial> materials;
    std::vector<CharBoneWeights> skin;   // empty: rigid on its frame
};

struct CharAnimTrack {
    int node = -1;
    std::vector<double> times;                   // ticks
    std::vector<std::array<double, 16>> keys;    // local matrices
};

struct CharAnim {
    std::string name;
    std::vector<CharAnimTrack> tracks;
    double ticks = 0;  // last key time
};

struct Character {
    std::string path;  // e.g. data/rena/00.X
    std::vector<CharNode> nodes;
    std::vector<CharMesh> meshes;
    std::vector<CharAnim> anims;
    double ticks_per_second = 4800;
    std::vector<std::string> errors;
};

bool load_character(const Source& a, const std::string& path, Character& out, std::string* err);

// Every data/<dir>/NN.X entry that holds skinned meshes is a character; this lists the
// names matching data/<dir>/0N.X (case-insensitive), sorted.
std::vector<std::string> list_characters(const Archive& a);

// The pose of animation `anim` (-1 = bind pose) at `seconds` (wrapped to the animation's
// length) as draw batches in slot 1, lit as described above. env is left empty (no fog).
StageGeometry pose_character(const Character& c, int anim, double seconds);

}  // namespace db
