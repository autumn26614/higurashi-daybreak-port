#include "fighter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "character.h"
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

void ease0(float& v, float step) {
    if (v > 0)
        v = std::max(0.f, v - step);
    else if (v < 0)
        v = std::min(0.f, v + step);
}

void to_air_hit(Fighter& f) {
    f.vy = 0;
    set_state(f, 103);
}

// The common reaction states (0x42F1C0): staggers, launches, lying down, getting up.
// Knock-back comes from the hit: speed = -knock[0] / 100 (away from the attacker, whom the
// fighter faces), vy = knock[1] / 100.
void reaction(Fighter& f) {
    const int s = f.state, st = f.step, fr = f.frame;
    const bool first = st == 0 && fr == 0;
    const bool landed = f.ground && f.vy < 0;
    switch (s) {
    case 100:
    case 101:
    case 102:
    case 130:
    case 131:
    case 134:
        if (done(f)) {
            set_state(f, s == 134 ? 120 : 0);
            return;
        }
        if (!f.ground) {
            if (s == 134) f.gravity = 0.5f;
            return to_air_hit(f);
        }
        if (fr == 0) {
            f.extra = 0;
            f.turn_rate = 0;
            f.speed = s <= 101 ? -15.f : (s == 102 || s == 134) ? -20.f : 0.f;
        }
        f.speed = std::min(0.f, f.speed + (s <= 101 ? 1.5f : (s == 102 || s == 134) ? 0.5f : 1.f));
        return;
    case 103:
        if (first) f.vy = 0;
        if (f.speed > 5) f.speed = std::max(5.f, f.speed - 0.25f);
        if (f.speed < -5) f.speed = std::min(-5.f, f.speed + 0.25f);
        f.gravity = 0.3f;
        f.vy -= f.gravity;
        if (f.ground && f.vy < 0) {
            f.vy = 0;
            set_state(f, 118);
        }
        return;
    case 104:
        if (done(f)) {
            set_state(f, 0);
            return;
        }
        if (!f.ground && st > 0) return to_air_hit(f);
        if (first) {
            f.extra = 0;
            f.speed = 0;
        }
        if (st == 0 && fr > 60) {
            set_step(f, 1);
            f.speed = -15;
            return;
        }
        f.speed = std::min(0.f, f.speed + 1.5f);
        return;
    case 105:
        if (first) {
            f.extra = 0;
            f.speed = 0;
            f.vy = 0;
        }
        if (st == 0 && fr > 60) {
            set_step(f, 1);
            f.speed = -f.knock[0] * 0.01f;
            f.vy = f.knock[1] * 0.01f;
            f.gravity = 0.3f;
            return;
        }
        if (landed && st > 0) {
            f.vy = 0;
            set_state(f, 118);
            return;
        }
        if (st > 0) f.vy -= f.gravity;
        if (f.speed < -4) f.speed += 0.25f;
        return;
    case 113:
    case 114:
    case 115:
    case 116:
        if (first) {
            f.extra = 0;
            f.turn_rate = 0;
            f.speed = -f.knock[0] * 0.01f;
            f.vy = f.knock[1] * 0.01f;
            f.gravity = s == 116 ? 1.f : 0.3f;
        }
        if (landed) {
            f.vy = 0;
            set_state(f, 118);
            return;
        }
        if (f.vy < 6 && st == 1 && (s != 115 || fr % 17 == 0)) {
            set_step(f, 2);
            return;
        }
        f.vy -= f.gravity;
        if (f.speed < -4) f.speed += 0.25f;
        return;
    case 118:
    case 119:
    case 121:
        if (done(f)) {
            if (f.hp > 0) set_state(f, 120);  // approx.: knocked out stays down
            return;
        }
        if (!f.ground) return to_air_hit(f);
        if (s == 121 && fr == 0) f.speed = -f.knock[0] * 0.01f;
        ease0(f.speed, 0.25f);
        return;
    case 120:
        if (done(f)) {
            set_state(f, f.in.x == 0 && f.in.y == 0 ? 128 : 124);
            return;
        }
        if (!f.ground) return to_air_hit(f);
        ease0(f.speed, 0.25f);
        return;
    case 124:
        if (done(f)) {
            set_state(f, 0);
            return;
        }
        if (!f.ground) return to_fall(f);
        regen(f);
        if (st == 0) {
            f.scratch = 0;
            f.extra = 0;
            f.turn_rate = 180;
            walk_heading(f);
        }
        if (st == 1) {
            f.turn_rate = 3;
            walk_heading(f);
            f.speed = 10;
            ++f.scratch;
            if (f.scratch > 150 || (f.scratch > 30 && (f.in.held[0] || f.in.held[1] || f.in.held[2]))) {
                set_step(f, 2);
                return;
            }
        }
        if (st == 2) {
            f.speed = 0;
            f.turn_rate = 6;
        }
        return;
    case 128:
        if (done(f)) {
            set_state(f, 0);
            return;
        }
        if (!f.ground) {
            f.vy = 0;
            return to_fall(f);
        }
        f.speed = 0;
        return;
    case 133:
        if (first) {
            f.extra = 0;
            f.speed = -f.knock[0] * 0.01f;
            f.vy = f.knock[1] * 0.01f;
            f.gravity = 0.3f;
            f.scratch = 0;
        }
        if (landed) {
            f.vy = 0;
            f.speed *= 0.5f;
            set_state(f, 118);
            return;
        }
        if (st == 1) {
            f.vy -= 0.3f;
            if (++f.scratch == 68) {
                set_step(f, 2);
                f.scratch = 0;
                return;
            }
        }
        if (st == 2) {
            f.vy -= 0.3f;
            if (f.speed < -4) f.speed += 0.25f;
        }
        return;
    default:  // reactions not decoded yet (guard 191+, throws)
        if (done(f)) {
            if (f.ground)
                set_state(f, 0);
            else
                to_fall(f);
            return;
        }
        ease0(f.speed, 0.5f);
        if (!f.ground) f.vy -= 0.5f;
        return;
    }
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
    default:
        break;
    }
    if (s >= 100 && s < 300) return reaction(f);
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

// Model space -> world, as place_character turns and moves a model.
void to_world(const Fighter& f, const float m[3], float out[3]) {
    const float yaw = f.body_yaw(), c = std::cos(yaw), s = std::sin(yaw);
    out[0] = m[0] * c + m[2] * s + f.pos[0];
    out[1] = m[1] + f.pos[1];
    out[2] = -m[0] * s + m[2] * c + f.pos[2];
}

// The attack sphere of a record: on its bone (offset pos, swept along size) when the model
// is known; approx. otherwise: in front of the attacker at chest height.
int sphere_points(const Fighter& a, const FrameRecord& rec, float pts[3][3]) {
    if (a.model && !rec.bone.empty()) {
        const float p0[3] = {float(rec.pos[0]), float(rec.pos[1]), float(rec.pos[2])};
        const float p1[3] = {p0[0] + float(rec.size[0]), p0[1] + float(rec.size[1]), p0[2] + float(rec.size[2])};
        float m0[3], m1[3];
        const double t = a.frame * kFrameSeconds;
        if (bone_point(*a.model, a.anim(), t, rec.bone, p0, m0) && bone_point(*a.model, a.anim(), t, rec.bone, p1, m1)) {
            const float mid[3] = {(m0[0] + m1[0]) / 2, (m0[1] + m1[1]) / 2, (m0[2] + m1[2]) / 2};
            to_world(a, m0, pts[0]);
            to_world(a, mid, pts[1]);
            to_world(a, m1, pts[2]);
            return 3;
        }
    }
    const float yaw = a.body_yaw(), reach = 50.f + float(rec.radius) + float(rec.size[1]) * 0.5f;
    pts[0][0] = a.pos[0] - std::sin(yaw) * reach;
    pts[0][1] = a.pos[1] + 90.f;
    pts[0][2] = a.pos[2] - std::cos(yaw) * reach;
    return 1;
}

// approx.: the defender is a vertical capsule (radius 40, 0..170 above its feet); its own hurt
// volume is not decoded. Lying down and getting up (118-128) cannot be hit.
bool touches(const Fighter& d, const float p[3], float radius) {
    const float dx = p[0] - d.pos[0], dz = p[2] - d.pos[2];
    const float y = p[1] - d.pos[1];
    const float dy = y < 0 ? -y : y > 170.f ? y - 170.f : 0.f;
    const float r = radius + 40.f;
    return dx * dx + dz * dz + dy * dy <= r * r;
}

bool has_hit_record(const Move& m) {
    for (const MoveStep& st : m.steps)
        for (const FrameRecord& r : st.frames)
            if (r.hit == 1) return true;
    return false;
}

// approx.: most characters' attacks hit through attack objects their own class code spawns
// (0x4256D0), not through the definition's records; until those are decoded, an attack move
// without any hit record hits in the first half of its last step with these values.
const FrameRecord& stand_in_hit() {
    static const FrameRecord r = [] {
        FrameRecord f;
        f.hit = 1;
        f.radius = 30;
        const int32_t vals[][2] = {{0, 1500}, {36, 6}, {40, 6}, {52, 100}, {56, 113}, {68, 500}, {72, 1000}};
        for (const auto& v : vals) std::memcpy(f.params.data() + v[0], &v[1], 4);
        return f;
    }();
    return r;
}

void hits(Fighter& a, Fighter& d) {
    if (d.state >= 118 && d.state <= 128) return;
    const Move* m = move_of(a, a.state);
    if (!m) return;
    const FrameRecord* rec = a.record();
    int serial;
    if (rec && rec->hit == 1) {
        serial = a.state * 10000 + a.step * 100 + int(rec - m->steps[size_t(a.step)].frames.data());
    } else {
        const int len = step_len(a);
        if (a.state < 300 || a.step != int(m->steps.size()) - 1 || a.frame < len / 4 || a.frame > len / 2 ||
            has_hit_record(*m))
            return;
        rec = &stand_in_hit();
        serial = a.state * 10000 + a.step * 100 + 99;
    }
    if (serial == a.hit_serial) return;
    float pts[3][3];
    const int n = sphere_points(a, *rec, pts);
    bool hit = false;
    for (int i = 0; i < n && !hit; ++i) hit = touches(d, pts[i], float(rec->radius));
    if (!hit) return;
    a.hit_serial = serial;
    d.hp = std::max(0, d.hp - std::max(0, rec->damage()));
    // The record names the reaction on the ground (params 52) and in the air (56), the
    // knock-back (68 / 72, x100) and the hit-stop of attacker and defender (36 / 40).
    d.knock[0] = float(rec->param(68));
    d.knock[1] = float(rec->param(72));
    int react = d.ground && d.hp > 0 ? rec->param(52) : rec->param(56);
    if (!move_of(d, react)) react = d.ground && d.hp > 0 ? 100 : 113;
    d.heading = 0;  // approx.: square up to the attacker
    d.extra = 0;
    if (!set_state(d, react)) set_state(d, 100);
    a.freeze = std::clamp(rec->param(36), 0, 30);
    d.freeze = std::clamp(rec->param(40), 0, 30);
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
                   std::vector<int> frames_b, const float spawn_b[3], const Character* model_a,
                   const Character* model_b) {
    *this = Battle{};
    const Character* models[2] = {model_a, model_b};
    const CharDef* defs[2] = {a, b};
    std::vector<int>* frames[2] = {&frames_a, &frames_b};
    const float* spawns[2] = {spawn_a, spawn_b};
    for (int i = 0; i < 2; ++i) {
        Fighter& f = this->f[size_t(i)];
        f.def = defs[i];
        f.model = models[i];
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
    // Hit-stop: a frozen fighter skips its update, movement and animation.
    bool frozen[2];
    for (int i = 0; i < 2; ++i) frozen[i] = f[size_t(i)].freeze > 0;
    for (int i = 0; i < 2; ++i)
        if (winner < 0 && !frozen[i]) commands(f[size_t(i)]);
    for (int i = 0; i < 2; ++i)
        if (!frozen[i]) update_state(f[size_t(i)], f[size_t(1 - i)]);
    for (int i = 0; i < 2; ++i)
        if (!frozen[i]) physics(f[size_t(i)]);
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
        hits(f[0], f[1]);
        hits(f[1], f[0]);
    }
    for (int i = 0; i < 2; ++i) {
        Fighter& me = f[size_t(i)];
        if (frozen[i])
            --me.freeze;
        else
            advance(me);
        // HP drain bar (0x422F60): follows HP down at 200 per frame.
        if (me.hp < me.hp_bar) me.hp_bar = std::max(me.hp, me.hp_bar - 200);
        if (me.hp == 0 && winner < 0) winner = 1 - i;
    }
    ++frame;
}

}  // namespace db::battle
