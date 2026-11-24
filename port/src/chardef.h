// Character definitions: data/<name>/NN.dat next to the model NN.X, read by the game's loader
// 0x421EA0 (analysis/11-battle-engine.md). Every base and Kai file parses to its last byte.
//
//   u32; char model[0x104]; @0x108 i32 head[6]
//   u32 nMoves; per move: i32 id, rank, cancel; u32 nSteps;
//     per step: i32 anim; u32 nFrames; per frame record (154 bytes):
//       char bone[30]; i32 from; i32 pos[3]; i32 radius; u8 params[80]; i32 flags; i32 f16;
//       i32 hit; i32 size[3]
//   u32 nAnims; per animation set: i32 blend, loop; u32 nEvents;
//     per event: i32 from, to, period; char texture[60]; char swap[60]
//
// A move is the state of the same number (0 idle, 10 walk, 20-27 dashes, 30 air dash, 40-44
// jump, 100+ hit reactions, 300+ attacks). Moves are chained by rank: a move can interrupt the
// current one when current.cancel <= next.rank and the current frame record is cancellable.
// A frame record applies from its `from` frame to the next record's; with a radius and damage
// it is an attack sphere of `radius` on `bone` (offset pos, swept along size), params hold
// its damage, reactions, knock-back and hit-stop.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace db {

struct FrameRecord {
    std::string bone;
    int32_t from = 0;
    std::array<int32_t, 3> pos{};
    int32_t radius = 0;
    std::array<uint8_t, 80> params{};
    int32_t flags = 0;  // 0x100 cancellable, 0x200 airborne
    int32_t f16 = 0;
    int32_t hit = 0;  // meaning unknown (1 on some attack records)
    std::array<int32_t, 3> size{};

    int32_t param(int offset) const;  // u32 at params[offset] (0 if out of range)
    int32_t damage() const { return param(0); }
    // An attack sphere is active from this record's frame to the next record's.
    bool attacks() const { return radius > 0 && damage() > 0; }
};

struct MoveStep {
    int32_t anim = 0;  // animation set, 0-based
    std::vector<FrameRecord> frames;
};

struct Move {
    int32_t id = -1;
    int32_t rank = 0;
    int32_t cancel = 0;
    std::vector<MoveStep> steps;
};

struct AnimEvent {
    int32_t from = 0, to = 0, period = 0;
    std::string texture, swap;  // e.g. an eye texture and the expression replacing it
};

struct AnimDef {
    int32_t blend = 0;  // cross-fade frames
    int32_t loop = 0;
    std::vector<AnimEvent> events;
};

struct CharDef {
    std::string model;
    std::array<int32_t, 6> head{};
    std::vector<Move> moves;
    std::vector<AnimDef> anims;

    const Move* move(int32_t id) const;  // nullptr when the character has no such move
};

bool parse_chardef(const uint8_t* data, size_t size, CharDef& out, std::string* err);

}  // namespace db
