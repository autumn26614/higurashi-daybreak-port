#include "fighter.h"

#include <algorithm>
#include <cmath>

#include "geometry.h"

namespace db::battle {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kRad = kPi / 180.f;

float wrap180(float a) {
    while (a < -180.f) a += 360.f;
    while (a > 180.f) a -= 360.f;
    return a;
}

const Move* move_of(const Fighter& f, int id) { return f.def ? f.def->move(id) : nullptr; }

int step_count(const Fighter& f) {
    const Move* m = move_of(f, f.state);
    return m ? int(m->steps.size()) : 0;
}

int step_len(const Fighter& f) {
    const int a = f.anim();
    if (a < 0 || size_t(a) >= f.anim_frames.size()) return 1;
    return std::max(1, f.anim_frames[size_t(a)]);
}

// SetStep 0x424650.
void set_step(Fighter& f, int n) {
    f.step = n;
    f.fresh = true;
    f.frame = 0;
}

// SetState 0x4245D0. A move the character does not have leaves the state unchanged.
bool set_state(Fighter& f, int s) {
    if (!move_of(f, s)) return false;
    f.state = s;
    f.hit_serial = -1;
    set_step(f, 0);
    return true;
}

// 0x424DE0: the last frame of the last step.
bool done(const Fighter& f) { return f.frame + 1 >= step_len(f) && f.step >= step_count(f) - 1; }

// 0x434B30: turn the heading toward target by at most rate degrees.
void turn(Fighter& f, float target, float rate) {
    const float d = wrap180(f.heading - target);
    if (std::fabs(d) < rate || rate >= 180.f) {
        f.heading = target;
        return;
    }
    f.heading = wrap180(d > 0 ? f.heading - rate : f.heading + rate);
}

// 0x424CA0: the stick picks a heading (up = toward the opponent, left positive).
void walk_heading(Fighter& f) {
    const int x = f.in.x, y = f.in.y;
    float t;
    if (x < 0)
        t = y < 0 ? 45.f : y == 0 ? 90.f : 135.f;
    else if (x == 0) {
        if (y == 0) return;
        t = y < 0 ? 0.f : 180.f;
    } else
        t = y < 0 ? -45.f : y == 0 ? -90.f : -135.f;
    turn(f, t, f.turn_rate);
}

void regen(Fighter& f) { f.stamina = std::min(1000, f.stamina + 40); }
void spend(Fighter& f, int n) { f.stamina = std::max(0, f.stamina - n); }

void to_fall(Fighter& f) {
    f.gravity = 0.5f;
    set_state(f, 43);
}

// Air control of the rising / falling states.
void drift(Fighter& f) {
    if (f.in.x == 0 && f.in.y == 0)
        f.speed = std::max(0.f, f.speed - 0.2f);
    else
        f.speed = std::min(6.f, f.speed + 0.2f);
}

float dist2(const Fighter& a, const Fighter& b) {
    const float dx = a.pos[0] - b.pos[0], dz = a.pos[2] - b.pos[2];
    return dx * dx + dz * dz;
}

int dir_index(int x, int y) {
    static const int table[3][3] = {{2, 4, 7}, {0, 10, 6}, {1, 3, 5}};  // [x+1][y+1], as state 10
    return table[(x > 0) - (x < 0) + 1][(y > 0) - (y < 0) + 1];
}

// Per-state update (0x443EE0). Returns after a state change, like the game.
void update_state(Fighter& f, const Fighter& o) {
    const int s = f.state, st = f.step, fr = f.frame;
    const bool first = st == 0 && fr == 0;
    switch (s) {
    case 0:
        f.speed = 0;
        regen(f);
        f.turn_rate = 0;
        f.scratch = 10;
        if (!f.ground) return to_fall(f);
        if (f.in.x || f.in.y) set_state(f, 10);
        return;
    case 10: {
        if (!f.ground) return to_fall(f);
        if (!f.in.x && !f.in.y) {
            set_state(f, 0);
            return;
        }
        const int d = dir_index(f.in.x, f.in.y);
        if (d != f.scratch) {
            f.scratch = d;
            f.turn_rate = 20;
        }
        f.turn_rate = std::max(1.f, f.turn_rate - 1.f);
        f.speed = 7.2f;
        regen(f);
        f.extra = 0;
        walk_heading(f);
        return;
    }
    case 20:
        if (done(f)) {
            if (!f.ground) return to_fall(f);
            set_state(f, 0);
            return;
        }
        if (!f.ground) {
            set_state(f, 30);
            return;
        }
        if (st < 2) spend(f, 12);
        if (first) {
            f.scratch = 0;
            f.extra = 0;
            if (std::fabs(f.heading) >= 12.f)
                f.heading += f.heading > 0 ? -10.f : 10.f;
            else
                f.heading = 0;
            f.speed = 17;
        }
        if (st == 1) {
            f.turn_rate = 4;
            f.speed = 17;
            walk_heading(f);
            if ((!f.in.x && !f.in.y) || f.stamina < 1) set_step(f, 2);
        }
        if (st == 2) {
            f.turn_rate = 0;
            f.speed = std::max(0.f, f.speed - 0.75f);
        }
        return;
    case 23:
    case 24:
    case 27: {
        if (done(f)) {
            set_state(f, 0);
            return;
        }
        if (!f.ground) return to_fall(f);
        const float side = s == 23 ? -90.f : s == 24 ? 90.f : 180.f;
        if (st < 2) {
            if (s != 27) turn(f, 0, 20);
            spend(f, 12);
        }
        if (first) {
            f.heading = s == 27 ? f.heading : 0;
            f.extra = side;
            f.speed = s == 27 ? 14.5f : 20.f;
        }
        if (st == 1) f.extra = side;
        if (s == 27) {
            if (st == 1) {
                f.turn_rate = 0;
                f.speed = 14.5f;
            }
            if (st == 2) f.speed = std::max(0.f, f.speed - 1.f);
        } else {
            if (st < 2) f.speed = std::max(12.5f, f.speed - 0.5f);
            if (st == 2) f.speed = std::max(0.f, f.speed - 0.7f);
        }
        return;
    }
    case 30:
        if (done(f)) return to_fall(f);
        if (st < 3) spend(f, 12);
        if (first) {
            f.speed = 16.5f;
            f.vy = 0;
            f.turn_rate = 60;
            f.extra = 0;
            f.scratch = 0;
        }
        if (st == 1) {
            f.turn_rate = 2;
            f.speed = 16.5f;
            ++f.scratch;
            if (f.scratch > 5 && ((!f.in.x && !f.in.y) || f.stamina < 1)) set_step(f, 2);
        }
        if (f.step == 2) {
            f.gravity = 0.5f;
            f.vy -= f.gravity;
            if (f.ground && f.vy <= 0) {
                f.vy = 0;
                set_state(f, 44);
                return;
            }
        }
        walk_heading(f);
        return;
    case 40:
        if (done(f)) {
            set_state(f, 41);
            return;
        }
        f.extra = 0;
        regen(f);
        f.gravity = 0;
        f.speed = 0;
        f.turn_rate = 18;
        walk_heading(f);
        return;
    case 41:
        if (first) {
            f.vy = 12;
            f.scratch = 0;
            if (f.in.x || f.in.y) f.speed = 6;
        }
        if (f.vy < 6 && st == 1) {
            set_state(f, 42);
            return;
        }
        if (f.ground && f.vy <= 0 && !first) {
            f.vy = 0;
            set_state(f, 44);
            return;
        }
        f.turn_rate = 1;
        f.vy -= f.gravity;
        ++f.scratch;
        if (f.gravity == 0) spend(f, 10);
        if (f.in.held[0] == 0) f.scratch = 0;
        if (f.scratch == 0 || f.scratch > 99 || f.stamina < 1) f.gravity = 0.5f;
        drift(f);
        walk_heading(f);
        return;
    case 42:
    case 43:
        if (f.ground && f.vy <= 0) {
            f.vy = 0;
            set_state(f, 44);
            return;
        }
        f.turn_rate = 1;
        if (f.gravity == 0) f.gravity = 0.5f;
        f.vy -= f.gravity;
        if (f.in.held[0] == 0 || f.in.held[0] > 59) f.gravity = 0.5f;
        if (s == 42) drift(f);
        walk_heading(f);
        return;
    case 44:
    case 329:
    case 333:
        if (done(f)) {
            set_state(f, 0);
            return;
        }
        f.gravity = 0;
        f.extra = 0;
        f.turn_rate = 0;
        f.speed = 0;
        regen(f);
        return;
    case 100:  // approx.: hit reaction
        f.speed = std::max(0.f, f.speed - 0.5f);
        if (!f.ground) f.vy -= 0.5f;
        if (done(f) && f.ground) {
            f.extra = 0;
            set_state(f, 0);
        }
        return;
    case 128:  // approx.: knocked out, stays down
        f.speed = std::max(0.f, f.speed - 0.5f);
        if (!f.ground) f.vy -= 0.5f;
        return;
    default:
        break;
    }
    // Attacks (300 and up); speeds from the Rena class, used for every character for now.
    const bool air = s == 320 || s == 332;
    if (done(f)) {
        if (air || !f.ground) return to_fall(f);
        set_state(f, 0);
        return;
    }
    if (!air && !f.ground) {
        f.gravity = 0.5f;
        set_state(f, 43);
        return;
    }
    if (first) {
        f.extra = 0;
        turn(f, 0, 180);
    }
    switch (s) {
    case 300:
        if (first) f.speed = 20;
        if (st < 2) turn(f, 0, 7);
        if (st == 1 && fr > 2 && (dist2(f, o) < 60000 || fr > 10)) set_step(f, 2);
        if (st == 2) f.speed = std::max(0.f, f.speed - 1.5f);
        break;
    case 301:
        if (fr < 8) turn(f, 0, 20);
        if (fr == 1) f.speed = 10;
        if (fr == 16) f.speed = 0;
        break;
    case 302:
        if (fr < 12) {
            f.speed = 0;
            turn(f, 0, 5);
        }
        if (fr == 12) f.speed = 10;
        if (fr == 22) f.speed = 0;
        break;
    case 310:
        if (first) f.speed = 25;
        if (st == 0) turn(f, 0, 20);
        if (st == 1) set_step(f, 2);
        if (st == 2) {
            f.speed = std::max(0.f, f.speed - 1.25f);
            if (fr == 5) f.speed = 0;
            if (fr < 5) turn(f, 0, 5);
        }
        break;
    case 311:
        if (fr == 0) f.speed = 20;
        if (st == 0) {
            f.speed = std::max(0.f, f.speed - 0.2f);
            if (fr < 20) turn(f, 0, 10);
        }
        if (fr == 25) f.speed = 0;
        break;
    case 320:
        if (f.ground && st == 2 && fr > 10) {
            f.speed = 0;
            f.vy = 0;
            set_state(f, 329);
            return;
        }
        if (first) {
            f.vy = std::clamp(f.vy, -3.f, 3.f);
            f.speed = 15;
        }
        if (st < 2) turn(f, 0, 3);
        if (st == 1 && fr > 10) set_step(f, 2);
        if (f.step == 2 && f.frame > 5) {
            if (f.gravity == 0) f.gravity = 0.5f;
            f.vy -= f.gravity;
        }
        break;
    case 330:
        if (st < 2) turn(f, 0, 5);
        if (st == 1) f.speed = 30;
        if (st == 2) f.speed = std::max(0.f, f.speed - 2.f);
        break;
    case 331:
        if (first) f.speed = 17.5f;
        if (st < 2) turn(f, 0, 20);
        if (st == 1 && fr > 2 && (dist2(f, o) < 90000 || fr > 20)) set_step(f, 2);
        if (st >= 2) f.speed = std::max(0.f, f.speed - 0.35f);
        break;
    case 332:
        if (f.ground && st > 0) {
            f.speed = 0;
            f.vy = 0;
            set_state(f, 333);
            return;
        }
        if (st == 0) {
            turn(f, 0, 5);
            f.speed = 0;
            f.vy = 0;
        }
        if (st == 1 && fr == 0) {  // dive at -12 degrees, 25 units per frame
            f.speed = 25.f * std::cos(12.f * kRad);
            f.vy = -25.f * std::sin(12.f * kRad);
        }
        if (st == 1 && fr > 40) {
            set_step(f, 2);
            f.speed = 10;
        }
        if (f.step == 2) {
            if (f.gravity == 0) f.gravity = 0.5f;
            f.vy -= f.gravity;
        }
        break;
    default:
        f.speed = std::max(0.f, f.speed - 1.f);
        break;
    }
}

bool pressed(const InputRecord& in, int b) {
    return in.held[size_t(b)] == 3 || (in.released[size_t(b)] > 0 && in.released[size_t(b)] < 3);
}

// Command / cancel layer (0x44B3F0), the parts reproduced so far.
void commands(Fighter& f) {
    const Move* cur = move_of(f, f.state);
    const FrameRecord* rec = f.record();
    const bool open = cur && rec && (rec->flags & 0x100);
    auto can = [&](int id) {
        const Move* m = move_of(f, id);
        return open && m && cur->cancel <= m->rank;
    };
    auto go = [&](int id) { return can(id) && set_state(f, id); };
    const int s = f.state;
    if (f.in.tap && f.stamina > 0) {
        static const int ground_dash[5] = {0, 20, 27, 24, 23};
        if (go(f.ground ? ground_dash[f.in.tap] : 30)) return;
    }
    if (pressed(f.in, 1)) {
        if (!f.ground) {
            if (s == 30 && f.step < 2 && go(332)) return;
            if (go(320)) return;
        } else {
            if (f.in.x && s == 20 && go(331)) return;
            if (s == 20 && go(330)) return;
            if (f.in.y < 0 && (s == 23 || s == 24) && go(330)) return;
            if ((s == 23 || s == 24) && go(331)) return;
            if (f.in.y < 0 && go(310)) return;
            if (f.in.x && go(311)) return;
            if ((s == 329 || s == 333 || s == 44) && go(340)) return;
            if (s == 340 && go(341)) return;
            if (s == 300 && go(301)) return;
            if (s == 301 && go(302)) return;
            if (go(300)) return;
        }
    }
    if (pressed(f.in, 0) && f.stamina > 0) {
        if ((f.in.x || f.in.y) && !f.ground && go(30)) return;
        if (f.ground && go(40)) return;
    }
}

void physics(Fighter& f) {
    const float yaw = (f.facing - (f.heading + f.extra)) * kRad;
    f.pos[0] += -std::sin(yaw) * f.speed;
    f.pos[2] += -std::cos(yaw) * f.speed;
    f.pos[1] += f.vy;
    if (f.ground) {
        if (f.vy > 0) f.ground = false;
    } else if (f.pos[1] <= f.floor) {
        f.pos[1] = f.floor;
        f.ground = true;
    }
    if (f.ground) f.pos[1] = f.floor;
}

// Steps through the animation; a looping animation repeats its step, otherwise the next
// step starts (the last one holds its final frame).
void advance(Fighter& f) {
    if (f.fresh) {
        f.fresh = false;
        return;
    }
    const int len = step_len(f);
    if (f.frame + 1 < len) {
        ++f.frame;
        return;
    }
    const int a = f.anim();
    const bool loop = f.def && a >= 0 && size_t(a) < f.def->anims.size() && f.def->anims[size_t(a)].loop;
    if (f.step + 1 < step_count(f) && !loop)
        set_step(f, f.step + 1);
    else if (loop || f.step + 1 >= step_count(f))
        f.frame = loop || f.state == 0 || f.state == 10 ? 0 : f.frame;
}

// approx.: the attack sphere sits in front of the attacker at chest height instead of on the
// animated bone; the defender is a vertical capsule.
void hits(Fighter& a, Fighter& d, int index_of_a) {
    (void)index_of_a;
    const FrameRecord* rec = a.record();
    if (!rec || rec->hit != 1 || d.state == 128) return;
    const Move* m = move_of(a, a.state);
    const int serial = a.state * 10000 + a.step * 100 + int(rec - m->steps[size_t(a.step)].frames.data());
    if (serial == a.hit_serial) return;
    const float yaw = a.body_yaw();
    const float reach = 50.f + float(rec->radius) + float(rec->size[1]) * 0.5f;
    const float cx = a.pos[0] - std::sin(yaw) * reach, cz = a.pos[2] - std::cos(yaw) * reach;
    const float cy = a.pos[1] + 90.f;
    const float dx = d.pos[0] - cx, dz = d.pos[2] - cz;
    const float dy = std::max(0.f, std::fabs(d.pos[1] + 90.f - cy) - 60.f);
    const float r = float(rec->radius) + 45.f;
    if (dx * dx + dz * dz + dy * dy > r * r) return;
    a.hit_serial = serial;
    d.hp = std::max(0, d.hp - std::max(0, rec->damage()));
    const bool ko = d.hp == 0;
    if (!set_state(d, ko ? 128 : 100)) set_state(d, 100);
    d.heading = 0;
    d.extra = 180;  // pushed away from the attacker (the defender faces it)
    d.speed = ko ? 10.f : 6.f;
    if (!d.ground) d.vy = std::max(d.vy, 3.f);
}

// approx.: a placeholder opponent, not the game's AI.
Pad cpu_pad(const Fighter& f, const Fighter& o, int frame) {
    Pad p;
    const float d2 = dist2(f, o);
    if (d2 > 350.f * 350.f) p.y = -1;
    if ((frame / 120) % 3 == 1) p.x = (frame / 360) % 2 ? 1 : -1;
    if (d2 < 280.f * 280.f && frame % 75 < 4) p.buttons |= 2;
    if (frame % 400 == 200) p.buttons |= 1;
    return p;
}

}  // namespace

void InputRecord::update(const Pad& p) {
    const int ox = x, oy = y;
    x = p.x < 0 ? (x < 0 ? x - 1 : -1) : p.x > 0 ? (x > 0 ? x + 1 : 1) : 0;
    y = p.y < 0 ? (y < 0 ? y - 1 : -1) : p.y > 0 ? (y > 0 ? y + 1 : 1) : 0;
    for (size_t i = 0; i < 4; ++i) {
        const bool down = (p.buttons >> i) & 1;
        released[i] = !down && held[i] > 0 ? held[i] : 0;
        held[i] = down ? held[i] + 1 : 0;
    }
    // Double tap: a direction pressed again within 12 frames of letting it go.
    const bool was[4] = {oy < 0, oy > 0, ox < 0, ox > 0};
    const bool now[4] = {y < 0, y > 0, x < 0, x > 0};
    tap = 0;
    for (size_t d = 0; d < 4; ++d) {
        if (now[d] && !was[d] && since_release[d] <= 12) tap = int(d) + 1;
        if (was[d] && !now[d])
            since_release[d] = 0;
        else if (!now[d])
            since_release[d] = std::min(99, since_release[d] + 1);
        else
            since_release[d] = 99;
    }
}

int Fighter::anim() const {
    const Move* m = def ? def->move(state) : nullptr;
    if (!m || m->steps.empty()) return 0;
    return m->steps[size_t(std::clamp(step, 0, int(m->steps.size()) - 1))].anim;
}

float Fighter::body_yaw() const { return (facing - heading) * kRad; }

const FrameRecord* Fighter::record() const {
    const Move* m = def ? def->move(state) : nullptr;
    if (!m || step < 0 || size_t(step) >= m->steps.size()) return nullptr;
    const FrameRecord* r = nullptr;
    for (const FrameRecord& fr : m->steps[size_t(step)].frames)
        if (fr.from <= frame) r = &fr;
    return r;
}

void Battle::start(const CharDef* a, std::vector<int> frames_a, const float spawn_a[3], const CharDef* b,
                   std::vector<int> frames_b, const float spawn_b[3]) {
    *this = Battle{};
    const CharDef* defs[2] = {a, b};
    std::vector<int>* frames[2] = {&frames_a, &frames_b};
    const float* spawns[2] = {spawn_a, spawn_b};
    for (int i = 0; i < 2; ++i) {
        Fighter& f = this->f[size_t(i)];
        f.def = defs[i];
        f.anim_frames = std::move(*frames[i]);
        for (int k = 0; k < 3; ++k) f.pos[k] = spawns[i][k];
        f.floor = spawns[i][1];
        set_state(f, 0);
    }
    for (int i = 0; i < 2; ++i)
        f[size_t(i)].facing = facing_yaw(f[size_t(i)].pos, f[size_t(1 - i)].pos) / kRad;
}

void Battle::step(const Pad* p1, const Pad* p2) {
    const Pad* pads[2] = {p1, p2};
    for (int i = 0; i < 2; ++i) {
        Fighter& me = f[size_t(i)];
        const Pad p = pads[i] ? *pads[i] : cpu_pad(me, f[size_t(1 - i)], frame);
        me.in.update(winner < 0 ? p : Pad{});
    }
    for (int i = 0; i < 2; ++i)
        f[size_t(i)].facing = facing_yaw(f[size_t(i)].pos, f[size_t(1 - i)].pos) / kRad;
    for (int i = 0; i < 2; ++i)
        if (winner < 0 && f[size_t(i)].state != 128) commands(f[size_t(i)]);
    for (int i = 0; i < 2; ++i) update_state(f[size_t(i)], f[size_t(1 - i)]);
    for (int i = 0; i < 2; ++i) physics(f[size_t(i)]);
    // Keep the two bodies apart.
    const float dx = f[1].pos[0] - f[0].pos[0], dz = f[1].pos[2] - f[0].pos[2];
    const float d = std::sqrt(dx * dx + dz * dz), min_d = 70.f;
    if (d < min_d && std::fabs(f[1].pos[1] - f[0].pos[1]) < 150.f) {
        const float ux = d > 0.01f ? dx / d : 1.f, uz = d > 0.01f ? dz / d : 0.f, push = (min_d - d) * 0.5f;
        f[0].pos[0] -= ux * push;
        f[0].pos[2] -= uz * push;
        f[1].pos[0] += ux * push;
        f[1].pos[2] += uz * push;
    }
    if (winner < 0) {
        hits(f[0], f[1], 0);
        hits(f[1], f[0], 1);
    }
    for (int i = 0; i < 2; ++i) {
        Fighter& me = f[size_t(i)];
        advance(me);
        // HP drain bar (0x422F60): follows HP down at 200 per frame.
        if (me.hp < me.hp_bar) me.hp_bar = std::max(me.hp, me.hp_bar - 200);
        if (me.hp == 0 && winner < 0) winner = 1 - i;
    }
    ++frame;
}

}  // namespace db::battle
