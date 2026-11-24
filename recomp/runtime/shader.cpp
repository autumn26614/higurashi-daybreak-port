#include "shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rt {
namespace shader {

namespace {
// Parameter tokens of each shader model 1 instruction (from 2.0 on, the token says).
int sm1_params(uint16_t op, bool pixel, int major, int minor) {
    switch (op) {
    case NOP: case PHASE: return 0;
    case MOV: case RCP: case RSQ: case EXP: case LOG: case LIT: case FRC: case EXPP: case LOGP: case ABS: case NRM:
    case MOVA: case DCL: return 2;
    case ADD: case SUB: case MUL: case DP3: case DP4: case MIN: case MAX: case SLT: case SGE: case DST: case M4x4:
    case M4x3: case M3x4: case M3x3: case M3x2: case POW: case BEM: return 3;
    case MAD: case LRP: case CND: case CMP: case DP2ADD: return 4;
    case DEF: return 5;
    case SINCOS: return major >= 3 ? 2 : 4;
    case TEXCOORD: return pixel && major == 1 && minor >= 4 ? 2 : 1;
    case TEX: return pixel && major == 1 && minor >= 4 ? 2 : (major >= 2 ? 3 : 1);
    case TEXKILL: return 1;
    case TEXBEM: case TEXBEML: case TEXREG2AR: case TEXREG2GB: case TEXM3x2PAD: case TEXM3x2TEX: case TEXM3x3PAD:
    case TEXM3x3TEX: case TEXM3x3VSPEC: return 2;
    case TEXM3x3SPEC: return 3;
    default: return -1;
    }
}

uint8_t reg_type(uint32_t t) { return uint8_t(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

Dst dst_of(uint32_t t) {
    Dst d;
    d.type = reg_type(t);
    d.num = uint16_t(t & 0x7FF);
    d.mask = uint8_t((t >> 16) & 0xF);
    d.sat = (t >> 20) & 1;
    int s = int((t >> 24) & 0xF);
    d.shift = int8_t(s >= 8 ? s - 16 : s);
    return d;
}

Src src_of(uint32_t t) {
    Src s;
    s.type = reg_type(t);
    s.num = uint16_t(t & 0x7FF);
    s.relative = (t >> 13) & 1;
    for (int i = 0; i < 4; i++) s.swz[i] = uint8_t((t >> (16 + 2 * i)) & 3);
    s.mod = uint8_t((t >> 24) & 0xF);
    return s;
}
}  // namespace

Program decode(const uint32_t *code, size_t n) {
    Program p;
    if (!n) {
        p.error = "empty";
        return p;
    }
    uint32_t ver = code[0];
    p.pixel = (ver >> 16) == 0xFFFF;
    p.major = uint8_t((ver >> 8) & 0xFF);
    p.minor = uint8_t(ver & 0xFF);
    size_t i = 1;
    while (i < n) {
        uint32_t t = code[i];
        if (t == 0x0000FFFF) break;
        uint16_t op = uint16_t(t & 0xFFFF);
        if (op == 0xFFFE) {  // comment
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        int params = p.major >= 2 ? int((t >> 24) & 0xF) : sm1_params(op, p.pixel, p.major, p.minor);
        if (params < 0) {
            p.error = "unknown opcode " + std::to_string(op);
            return p;
        }
        Ins ins;
        ins.op = op;
        ins.coissue = (t >> 30) & 1;
        size_t k = i + 1;
        if (op == DCL) {
            uint32_t u = code[k];
            ins.usage = u & 0x1F;
            ins.usage_index = (u >> 16) & 0xF;
            ins.d = dst_of(code[k + 1]);
            if (!p.pixel && ins.d.type == INPUT) p.inputs.push_back({ins.d.num, ins.usage, ins.usage_index});
        } else if (op == DEF) {
            ins.d = dst_of(code[k]);
            memcpy(ins.def, &code[k + 1], 16);
            p.defs.push_back({ins.d.num, {ins.def[0], ins.def[1], ins.def[2], ins.def[3]}});
        } else if (params > 0) {
            ins.d = dst_of(code[k++]);
            for (int s = 0; s < params - 1 && s < 3; s++) {
                ins.s[s] = src_of(code[k++]);
                if (ins.s[s].relative && p.major >= 2) k++;  // the address register token
            }
            ins.nsrc = std::min(params - 1, 3);
        }
        p.ins.push_back(ins);
        i += 1 + size_t(params);
    }
    return p;
}

// ---------------------------------------------------------------------------------------
namespace {
float dp3(const Vec4 &a, const Vec4 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float dp4(const Vec4 &a, const Vec4 &b) { return dp3(a, b) + a[3] * b[3]; }
Vec4 splat(float f) { return {f, f, f, f}; }

Vec4 modify(Vec4 v, const Src &s) {
    Vec4 r{v[s.swz[0]], v[s.swz[1]], v[s.swz[2]], v[s.swz[3]]};
    for (float &x : r) {
        switch (s.mod) {
        case 1: x = -x; break;
        case 2: x = x - 0.5f; break;
        case 3: x = -(x - 0.5f); break;
        case 4: x = 2 * (x - 0.5f); break;
        case 5: x = -2 * (x - 0.5f); break;
        case 6: x = 1 - x; break;
        case 7: x = 2 * x; break;
        case 8: x = -2 * x; break;
        case 11: x = std::fabs(x); break;
        case 12: x = -std::fabs(x); break;
        default: break;
        }
    }
    return r;
}

struct Regs {
    Vec4 r[32]{};
    Vec4 t[8]{};
    const Vec4 *v = nullptr;  // inputs
    int nv = 0;
    const float *c = nullptr;
    int nc = 0;
    const Program *p = nullptr;
    int a0 = 0;
    Vec4 o_pos{0, 0, 0, 1}, o_color[2]{{1, 1, 1, 1}, {0, 0, 0, 0}}, o_tex[8]{};
    float o_fog = 1, o_psize = 1;

    Vec4 constant(int n) const {
        for (auto &d : p->defs)
            if (int(d.first) == n) return d.second;
        if (n < 0 || n >= nc) return {0, 0, 0, 0};
        return {c[4 * n], c[4 * n + 1], c[4 * n + 2], c[4 * n + 3]};
    }
    Vec4 read(const Src &s) const {
        Vec4 v{0, 0, 0, 0};
        switch (s.type) {
        case TEMP: v = r[s.num & 31]; break;
        case INPUT: v = s.num < nv ? this->v[s.num] : Vec4{0, 0, 0, 0}; break;
        case CONST: v = constant(int(s.num) + (s.relative ? a0 : 0)); break;
        case ADDR_OR_TEX: v = p->pixel ? t[s.num & 7] : splat(float(a0)); break;
        default: break;
        }
        return modify(v, s);
    }
    void write(const Dst &d, Vec4 v, bool pixel) {
        if (d.shift) {
            float f = d.shift > 0 ? float(1 << d.shift) : 1.0f / float(1 << -d.shift);
            for (float &x : v) x *= f;
        }
        if (d.sat)
            for (float &x : v) x = std::min(1.0f, std::max(0.0f, x));
        if (pixel)
            for (float &x : v) x = std::min(8.0f, std::max(-8.0f, x));
        Vec4 *target = nullptr;
        switch (d.type) {
        case TEMP: target = &r[d.num & 31]; break;
        case ADDR_OR_TEX:
            if (pixel) target = &t[d.num & 7];
            else {
                a0 = int(std::floor(v[0]));
                return;
            }
            break;
        case RASTOUT:
            if (d.num == 0) target = &o_pos;
            else {
                if (d.num == 1) o_fog = v[0];
                else o_psize = v[0];
                return;
            }
            break;
        case ATTROUT: target = &o_color[d.num & 1]; break;
        case TEXCRDOUT: target = &o_tex[d.num & 7]; break;
        default: return;
        }
        for (int i = 0; i < 4; i++)
            if (d.mask & (1 << i)) (*target)[i] = v[i];
    }
};

// Shared arithmetic; returns false for instructions it does not handle.
bool arith(Regs &R, const Ins &in, bool pixel) {
    auto S = [&](int i) { return R.read(in.s[i]); };
    Vec4 r{};
    switch (in.op) {
    case MOV: case MOVA: r = S(0); break;
    case ADD: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; break; }
    case SUB: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = a[i] - b[i]; break; }
    case MUL: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; break; }
    case MAD: { Vec4 a = S(0), b = S(1), c = S(2); for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; break; }
    case DP3: r = splat(dp3(S(0), S(1))); break;
    case DP4: r = splat(dp4(S(0), S(1))); break;
    case MIN: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = std::min(a[i], b[i]); break; }
    case MAX: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = std::max(a[i], b[i]); break; }
    case SLT: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.0f : 0.0f; break; }
    case SGE: { Vec4 a = S(0), b = S(1); for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.0f : 0.0f; break; }
    case RCP: { float x = S(0)[3]; r = splat(x == 1.0f ? 1.0f : 1.0f / x); break; }
    case RSQ: { float x = std::fabs(S(0)[3]); r = splat(x == 1.0f ? 1.0f : 1.0f / std::sqrt(x)); break; }
    case EXP: r = splat(std::exp2(S(0)[3])); break;
    case LOG: { float x = std::fabs(S(0)[3]); r = splat(x == 0 ? -INFINITY : std::log2(x)); break; }
    case EXPP: {
        float x = S(0)[3], fl = std::floor(x);
        r = {std::exp2(fl), x - fl, std::exp2(x), 1.0f};
        break;
    }
    case LOGP: {
        float x = std::fabs(S(0)[3]);
        if (x == 0) r = splat(-INFINITY);
        else {
            int e;
            float m = std::frexp(x, &e);
            r = {float(e - 1), m * 2, std::log2(x), 1.0f};
        }
        break;
    }
    case LIT: {
        Vec4 a = S(0);
        float pw = std::max(-128.0f, std::min(128.0f, a[3]));
        r = {1.0f, std::max(a[0], 0.0f), a[0] > 0 ? std::pow(std::max(a[1], 0.0f), pw) : 0.0f, 1.0f};
        break;
    }
    case DST: { Vec4 a = S(0), b = S(1); r = {1.0f, a[1] * b[1], a[2], b[3]}; break; }
    case LRP: { Vec4 a = S(0), b = S(1), c = S(2); for (int i = 0; i < 4; i++) r[i] = a[i] * (b[i] - c[i]) + c[i]; break; }
    case FRC: { Vec4 a = S(0); for (int i = 0; i < 4; i++) r[i] = a[i] - std::floor(a[i]); break; }
    case POW: r = splat(std::pow(std::fabs(S(0)[3]), S(1)[3])); break;
    case ABS: { Vec4 a = S(0); for (int i = 0; i < 4; i++) r[i] = std::fabs(a[i]); break; }
    case NRM: {
        Vec4 a = S(0);
        float l = std::sqrt(dp3(a, a));
        float k = l > 0 ? 1.0f / l : 0.0f;
        r = {a[0] * k, a[1] * k, a[2] * k, a[3] * k};
        break;
    }
    case SINCOS: { float x = S(0)[3]; r = {std::cos(x), std::sin(x), 0, 0}; break; }
    case CND: { Vec4 a = S(0), b = S(1), c = S(2); for (int i = 0; i < 4; i++) r[i] = a[i] > 0.5f ? b[i] : c[i]; break; }
    case CMP: { Vec4 a = S(0), b = S(1), c = S(2); for (int i = 0; i < 4; i++) r[i] = a[i] >= 0 ? b[i] : c[i]; break; }
    case DP2ADD: { Vec4 a = S(0), b = S(1), c = S(2); r = splat(a[0] * b[0] + a[1] * b[1] + c[0]); break; }
    case M4x4: case M4x3: case M3x4: case M3x3: case M3x2: {
        int rows = in.op == M4x4 || in.op == M3x4 ? 4 : in.op == M3x2 ? 2 : 3;
        bool four = in.op == M4x4 || in.op == M4x3;
        Vec4 a = S(0);
        for (int k = 0; k < rows; k++) {
            Src row = in.s[1];
            row.num = uint16_t(row.num + k);
            Vec4 b = R.read(row);
            r[k] = four ? dp4(a, b) : dp3(a, b);
        }
        Dst d = in.d;
        d.mask &= uint8_t((1 << rows) - 1);
        R.write(d, r, pixel);
        return true;
    }
    default:
        return false;
    }
    R.write(in.d, r, pixel);
    return true;
}
}  // namespace

void run_vs(const Program &p, const float *consts, const VSIn &in, VSOut &out) {
    Regs R;
    R.p = &p;
    R.v = in.v;
    R.nv = 16;
    R.c = consts;
    R.nc = 256;
    for (const Ins &ins : p.ins) {
        if (ins.op == DCL || ins.op == DEF || ins.op == NOP) continue;
        arith(R, ins, false);
    }
    out.pos = R.o_pos;
    out.color[0] = R.o_color[0];
    out.color[1] = R.o_color[1];
    for (int i = 0; i < 8; i++) out.tex[i] = R.o_tex[i];
    out.fog = R.o_fog;
    out.psize = R.o_psize;
}

bool run_ps(const Program &p, const float *consts, const PSIn &in, Sampler sample, void *ctx, Vec4 &out) {
    Regs R;
    R.p = &p;
    R.v = in.color;
    R.nv = 2;
    R.c = consts;
    R.nc = 32;
    bool ps14 = p.major == 1 && p.minor >= 4;
    for (const Ins &ins : p.ins) {
        switch (ins.op) {
        case DCL: case DEF: case NOP: case PHASE: continue;
        case TEX: {
            int stage = ins.d.num & 7;
            Vec4 tc = in.tex[stage];
            if (ps14 && ins.nsrc >= 1) {
                const Src &s = ins.s[0];
                tc = s.type == ADDR_OR_TEX ? in.tex[s.num & 7] : R.read(s);
            }
            Vec4 v = sample(ctx, stage, tc[0], tc[1]);
            Dst d = ins.d;
            if (!ps14) d.type = ADDR_OR_TEX;
            R.write(d, v, true);
            continue;
        }
        case TEXCOORD: {
            Vec4 tc = in.tex[(ps14 && ins.nsrc >= 1 ? ins.s[0].num : ins.d.num) & 7];
            for (int i = 0; i < 3; i++) tc[i] = std::min(1.0f, std::max(0.0f, tc[i]));
            tc[3] = 1;
            R.write(ins.d, tc, true);
            continue;
        }
        case TEXKILL: {
            Vec4 v = ins.d.type == ADDR_OR_TEX && !ps14 ? in.tex[ins.d.num & 7] : R.r[ins.d.num & 31];
            if (v[0] < 0 || v[1] < 0 || v[2] < 0) return false;
            continue;
        }
        default:
            arith(R, ins, true);
        }
    }
    out = R.r[0];
    for (float &x : out) x = std::min(1.0f, std::max(0.0f, x));
    return true;
}

}  // namespace shader
}  // namespace rt
