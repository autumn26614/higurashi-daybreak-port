// Battle core: two fighters driven by the game's state machine, as decoded from the
// character class at vtable 0x62E540 (per-state update 0x443EE0, command / cancel layer
// 0x44B3F0, input 0x422AA0); see analysis/11-battle-engine.md. One step() is one game frame.
//
// Reproduced: input record (held-frame counts, release lengths, double taps), the move table
// from the character definition (steps, ranks, cancel levels, cancellable frames), idle, walk,
// the four ground dashes, air dash, jump (hold to float), fall, landing, the melee chain and
// the dash / air attacks with the game's speeds, stamina, HP and the HP drain bar; hits with
// the attack record's damage, hit-stop, knock-back and reaction state (ground / air), and the
// common reaction states of 0x42F1C0 (stagger, launch, down, get up, roll). Attack spheres
// sit on the animated bone when the character model is given.
// The floor follows the stage's collision model (slopes, ledges) when one is given.
// Approximated (marked "approx." in the code): no walls yet, the defender is a capsule (its hurt volume is not
// decoded), a knocked-out fighter stays down, attack moves without any attack record (shots,
// throws, specials in the character's own code) use a stand-in hit, every character moves
// with Rena's attack speeds, and the computer opponent is a simple walker, not the
// game's AI (vtable +0x14). Not yet: shots, specials, guard, items, effects, sounds.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "chardef.h"

namespace db {
struct Character;
}

namespace db::battle {

constexpr double kFrameSeconds = 0.016;  // the game advances animations 0.016 s per frame

// One frame of controls: x / y in -1..1 (x < 0 left, y < 0 up = toward the opponent),
// buttons bit i = button i held (0 jump / boost, 1 melee, 2 shot, 3 special).
struct Pad {
    int x = 0, y = 0;
    uint32_t buttons = 0;
};

// The game's per-player input record (+0x274): signed held-frame counts for the axes,
// held-frame counts for buttons, release lengths and a double-tap code.
struct InputRecord {
    int x = 0, y = 0;
    std::array<int, 4> held{};
    std::array<int, 4> released{};  // on the release frame: how long it was held
    int tap = 0;                     // 1 up, 2 down, 3 left, 4 right (this frame only)
    std::array<int, 4> since_release{{99, 99, 99, 99}};  // frames since each direction let go
    void update(const Pad& p);
};

// The stage's character collision model (data/stageNN/collision.x) as world triangles.
// The game ray casts it straight down from 50 above the feet (0x433DA0); a triangle whose
// vertex colour has blue < 0x80 is not floor (the game flags it as a special surface).
struct Floor {
    std::vector<float> tris;  // 9 floats per triangle
    // Highest floor at or below y + 50 under (x, z); false if there is none.
    bool below(float x, float y, float z, float& out) const;
    bool empty() const { return tris.empty(); }
};

struct Fighter {
    const CharDef* def = nullptr;
    std::vector<int> anim_frames;  // length of each animation set in frames

    float pos[3]{};
    float floor = 0;      // floor height under the fighter (spawn height until a floor is set)
    float vy = 0;         // +0x40
    float speed = 0;      // +0x54
    float gravity = 0;    // +0x58
    float facing = 0;     // +0x5C, degrees, toward the opponent
    float extra = 0;      // +0x60
    float heading = 0;    // +0x64, relative to facing, left positive
    float turn_rate = 0;  // +0x4130
    bool ground = true;   // +0x84
    int scratch = 0;      // +0xE4
    int state = 0, step = 0, frame = 0;
    int hp = 10000, hp_bar = 10000, stamina = 1000;
    int hit_serial = -1;  // the attack record that already hit (once per record)
    int freeze = 0;       // hit-stop frames left
    float knock[2]{};     // +0x4134 / +0x4138: knock-back of the last hit (x100)
    const Character* model = nullptr;  // skeleton for attack spheres, or nullptr
    bool fresh = false;   // the step started this frame: its frame 0 is not skipped
    InputRecord in;

    int anim() const;           // animation set of the current step
    float body_yaw() const;     // radians, for place_character
    const FrameRecord* record() const;  // frame record in effect, or nullptr
};

// The battle camera (0x5B1400): behind `me`, looking at a point between `me` and `them` that
// moves out as they separate (`distance` is the camera's +0x78, 300 in the game).
void follow_camera(const float me[3], const float them[3], float distance, float eye[3], float target[3]);

struct Battle {
    std::array<Fighter, 2> f;
    int frame = 0;
    int winner = -1;  // set when a fighter is knocked out
    Floor floor;      // empty: flat floor at each spawn height

    // anim_frames: per character, the frame count of each of its model's animation sets.
    // models: the skeletons for attack spheres (nullptr: spheres in front of the attacker).
    void start(const CharDef* a, std::vector<int> frames_a, const float spawn_a[3], const CharDef* b,
               std::vector<int> frames_b, const float spawn_b[3], const Character* model_a = nullptr,
               const Character* model_b = nullptr);
    // Uses the stage floor from now on and stands both fighters on it.
    void set_floor(Floor fl);
    // pads[i] == nullptr: that fighter is computer controlled.
    void step(const Pad* p1, const Pad* p2);
};

}  // namespace db::battle
