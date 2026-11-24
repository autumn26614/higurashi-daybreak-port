// Character definition parser and the battle state machine, on a synthetic character.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/chardef.h"
#include "../src/fighter.h"

namespace {

int failures = 0;

// Puts a fighter in a state directly (what the game's SetState does).
void set_state_for_test(db::battle::Fighter& f, int st) {
    f.state = st;
    f.step = 0;
    f.frame = 0;
}

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Writer {
    std::vector<uint8_t> b;
    void i32(int32_t v) {
        uint8_t t[4];
        std::memcpy(t, &v, 4);
        b.insert(b.end(), t, t + 4);
    }
    void str(const std::string& s, size_t n) {
        std::vector<uint8_t> t(n, 0);
        std::memcpy(t.data(), s.data(), std::min(n - 1, s.size()));
        b.insert(b.end(), t.begin(), t.end());
    }
};

struct Rec {
    int from, flags, hit = 0, radius = 0, damage = 0;
    int ground = 100, air = 113, kx = 500, ky = 500, stop = 6;
};
struct StepSpec {
    int anim;
    std::vector<Rec> recs;
};
struct MoveSpec {
    int id, rank, cancel;
    std::vector<StepSpec> steps;
};

std::vector<uint8_t> build(const std::vector<MoveSpec>& moves, int nanims, const std::vector<int>& loops) {
    Writer w;
    w.i32(0);
    w.str("00.x", 0x104);
    for (int v : {850, 900, 0, 1, 850, 900}) w.i32(v);
    w.i32(int32_t(moves.size()));
    for (const auto& m : moves) {
        w.i32(m.id);
        w.i32(m.rank);
        w.i32(m.cancel);
        w.i32(int32_t(m.steps.size()));
        for (const auto& s : m.steps) {
            w.i32(s.anim);
            w.i32(int32_t(s.recs.size()));
            for (const auto& r : s.recs) {
                w.str(r.hit ? "Bip01_Prop1" : "", 30);
                w.i32(r.from);
                for (int k = 0; k < 3; ++k) w.i32(0);
                w.i32(r.radius);
                uint8_t params[80] = {};
                std::memcpy(params, &r.damage, 4);
                if (r.hit) {
                    std::memcpy(params + 36, &r.stop, 4);
                    std::memcpy(params + 40, &r.stop, 4);
                    std::memcpy(params + 52, &r.ground, 4);
                    std::memcpy(params + 56, &r.air, 4);
                    std::memcpy(params + 68, &r.kx, 4);
                    std::memcpy(params + 72, &r.ky, 4);
                }
                w.b.insert(w.b.end(), params, params + 80);
                w.i32(r.flags);
                w.i32(0);
                w.i32(r.hit);
                w.i32(0);
                w.i32(r.hit ? 100 : 0);
                w.i32(0);
            }
        }
    }
    w.i32(nanims);
    for (int i = 0; i < nanims; ++i) {
        w.i32(1);
        w.i32(std::find(loops.begin(), loops.end(), i) != loops.end() ? 1 : 0);
        if (i == 0) {
            w.i32(1);
            w.i32(0);
            w.i32(10);
            w.i32(60);
            w.str("eye.bmp", 60);
            w.str("eye2.bmp", 60);
        } else {
            w.i32(0);
        }
    }
    return w.b;
}

std::vector<MoveSpec> moves() {
    const Rec open{0, 0x100};
    const Rec shut{0, 0};
    auto three = [&](int id, int a) {
        return MoveSpec{id, 0, 5, {{a, {open}}, {a + 1, {open}}, {a + 2, {shut}}}};
    };
    return {
        {0, 0, 0, {{0, {open}}}},
        {10, 0, 0, {{1, {open}}}},
        three(20, 2),
        three(23, 2),
        three(24, 2),
        three(27, 2),
        {30, 0, 10, {{2, {shut}}, {3, {shut}}, {4, {shut}}}},
        {40, 5, 0, {{5, {shut}}}},
        {41, 0, 0, {{6, {shut}}, {6, {shut}}}},
        {42, 0, 0, {{6, {shut}}}},
        {43, 0, 0, {{6, {shut}}}},
        {44, 0, 5, {{7, {shut, {10, 0x100}}}}},
        {100, 0, 99, {{8, {shut}}}},
        {103, 0, 99, {{8, {shut}}}},
        {113, 0, 99, {{8, {shut}}, {8, {shut}}, {8, {shut}}}},
        {118, 0, 99, {{8, {shut}}}},
        {120, 0, 99, {{8, {shut}}}},
        {124, 0, 99, {{8, {shut}}, {8, {shut}}, {8, {shut}}}},
        {128, 0, 99, {{8, {shut}}}},
        {300, 10, 20, {{9, {shut}}, {9, {shut}}, {9, {shut, {4, 0, 1, 20, 1500}, {10, 0}, {15, 0x100}}}}},
        {301, 20, 30, {{9, {shut, {7, 0, 1, 20, 1500}, {16, 0}, {20, 0x100}}}}},
    };
}

}  // namespace

int main() {
    using namespace db;
    using namespace db::battle;
    const std::vector<uint8_t> bytes = build(moves(), 12, {0, 1, 3});

    CharDef def;
    std::string err;
    check(parse_chardef(bytes.data(), bytes.size(), def, &err), "parses");
    check(def.model == "00.x" && def.head[0] == 850 && def.head[5] == 900, "header");
    check(def.moves.size() == moves().size() && def.anims.size() == 12, "counts");
    const Move* m300 = def.move(300);
    check(m300 && m300->rank == 10 && m300->cancel == 20 && m300->steps.size() == 3, "move 300");
    check(m300 && m300->steps[2].frames[1].hit == 1 && m300->steps[2].frames[1].damage() == 1500 &&
              m300->steps[2].frames[1].bone == "Bip01_Prop1",
          "attack record");
    check(def.anims[0].loop == 1 && def.anims[2].loop == 0 && def.anims[0].events.size() == 1 &&
              def.anims[0].events[0].swap == "eye2.bmp" && def.anims[0].events[0].period == 60,
          "animation sets");
    check(!def.move(999), "missing move");
    CharDef bad;
    check(!parse_chardef(bytes.data(), bytes.size() - 1, bad, &err), "truncated fails");
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    check(!parse_chardef(longer.data(), longer.size(), bad, &err), "trailing bytes fail");

    const std::vector<int> frames(12, 20);
    const float a[3] = {0, 0, 0}, b[3] = {0, 0, -800};
    Battle bt;
    auto fresh = [&] { bt.start(&def, frames, a, &def, frames, b); };
    const Pad idle{};
    auto run = [&](const Pad& p, int n) {
        for (int i = 0; i < n; ++i) bt.step(&p, &idle);
    };
    auto dist = [&] {
        return std::hypot(bt.f[0].pos[0] - bt.f[1].pos[0], bt.f[0].pos[2] - bt.f[1].pos[2]);
    };

    // Walk toward the opponent.
    fresh();
    check(bt.f[0].state == 0, "starts idle");
    const float d0 = dist();
    run(Pad{0, -1, 0}, 10);
    check(bt.f[0].state == 10, "stick -> walk");
    check(std::fabs((d0 - dist()) - 7.2f * 9) < 1.f, "walks 7.2 per frame toward the opponent");
    run(idle, 1);
    check(bt.f[0].state == 0, "release -> idle");

    // Walking left moves to the fighter's left: facing -z, left is +x (the game's left-handed axes).
    fresh();
    run(Pad{-1, 0, 0}, 30);
    check(bt.f[0].pos[0] > 100.f && bt.f[0].pos[0] > std::fabs(bt.f[0].pos[2]), "left walks along +x");

    // Double tap up: forward dash.
    fresh();
    run(Pad{0, -1, 0}, 1);
    run(idle, 2);
    run(Pad{0, -1, 0}, 1);
    check(bt.f[0].state == 20 && std::fabs(bt.f[0].speed - 17.f) < 0.01f, "double tap -> dash at 17");
    check(bt.f[0].stamina < 1000, "dash costs stamina");

    // Jump: button 0 for 3 frames -> squat, rise at 12 per frame, fall, land, idle.
    fresh();
    run(Pad{0, 0, 1}, 3);
    check(bt.f[0].state == 40, "jump squat");
    run(idle, 20);
    check(bt.f[0].state == 41 || bt.f[0].state == 42, "rising");
    float top = 0;
    int landed = -1;
    for (int i = 0; i < 200 && landed < 0; ++i) {
        run(idle, 1);
        top = std::max(top, bt.f[0].pos[1]);
        if (bt.f[0].state == 44) landed = i;
    }
    check(top > 100.f && landed > 0 && bt.f[0].ground && bt.f[0].pos[1] == 0.f, "jumps and lands");
    run(idle, 40);
    check(bt.f[0].state == 0, "back to idle after landing");

    // Melee near the opponent hits once for 1500: hit-stop, the ground reaction (100) pushes the
    // defender back, it recovers to idle.
    const float near[3] = {0, 0, -150};
    bt.start(&def, frames, a, &def, frames, near);
    run(Pad{0, 0, 2}, 3);
    check(bt.f[0].state == 300, "melee button -> 300");
    for (int i = 0; i < 80 && bt.f[1].hp == 10000; ++i) run(idle, 1);
    check(bt.f[1].hp == 8500, "one hit for 1500");
    check(bt.f[1].state == 100, "ground reaction from the record");
    check(bt.f[0].freeze > 0 && bt.f[1].freeze > 0, "hit-stop on both");
    const float before = dist();
    run(idle, 20);
    check(dist() > before + 10.f, "knocked back");
    run(idle, 60);
    check(bt.f[1].hp == 8500, "still one hit");
    check(bt.f[1].state == 0, "recovers");
    check(bt.f[1].hp_bar == bt.f[1].hp, "drain bar catches up");

    // Hit in the air: the air reaction (113) launches by the knock-back, lands lying down (118),
    // gets up (120 -> 128 without input) and stands.
    bt.start(&def, frames, a, &def, frames, b);
    bt.f[1].knock[0] = 500;
    bt.f[1].knock[1] = 1000;
    set_state_for_test(bt.f[1], 113);
    float peak = 0;
    std::vector<int> seen;
    for (int i = 0; i < 400; ++i) {
        run(idle, 1);
        if (seen.empty() || seen.back() != bt.f[1].state) seen.push_back(bt.f[1].state);
        peak = std::max(peak, bt.f[1].pos[1]);
    }
    auto saw = [&](int st) { return std::find(seen.begin(), seen.end(), st) != seen.end(); };
    check(saw(113) && saw(118) && saw(120) && saw(128) && bt.f[1].state == 0 && peak > 100.f,
          "launch (vy 10), down, get up");

    // Knock-out: launched, stays down, the round is over.
    bt.start(&def, frames, a, &def, frames, near);
    bt.f[1].hp = 1000;
    run(Pad{0, 0, 2}, 3);
    run(idle, 200);
    check(bt.winner == 0 && bt.f[1].hp == 0 && bt.f[1].state == 118, "knock-out stays down");

    // The computer opponent moves on its own.
    fresh();
    for (int i = 0; i < 120; ++i) bt.step(&idle, nullptr);
    check(dist() < 790.f, "computer walks in");

    // Input record: held counts, release lengths, double taps.
    InputRecord in;
    in.update(Pad{-1, 0, 1});
    in.update(Pad{-1, 0, 1});
    check(in.x == -2 && in.held[0] == 2, "held counts");
    in.update(Pad{0, 0, 0});
    check(in.released[0] == 2 && in.x == 0, "release length");
    in.update(Pad{-1, 0, 0});
    check(in.tap == 3, "double tap left");

    if (failures == 0) std::printf("battle_test: all passed\n");
    return failures ? 1 : 0;
}
