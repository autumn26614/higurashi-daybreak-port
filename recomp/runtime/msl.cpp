#include "msl.h"

#include <cstdio>
#include <map>
#include <sstream>

namespace rt {
namespace msl {

using namespace shader;

std::string prelude() {
    return R"MSL(#include <metal_stdlib>
using namespace metal;

// What every vertex function writes and every fragment function reads.
struct VOut {
    float4 pos [[position]];
    float4 c0;
    float4 c1;
    float4 t0; float4 t1; float4 t2; float4 t3; float4 t4; float4 t5; float4 t6; float4 t7;
    float fog;      // vertex fog factor (1: no fog)
    float vdepth;   // eye-space depth, for table fog
    float psize [[point_size]];
};

// Per draw (vertex buffer 18, fragment buffer 1). All members are 16-byte vectors so the
// C++ mirror (metal.mm) has the same layout.
struct DrawBlock {
    float4 viewport;      // x, y, width, height in pixels
    float4 fog;           // start, end, density, mode (0 none, 1 exp, 2 exp2, 3 linear)
    float4 fog_color;
    float4 tfactor;
    float4 alpha;         // reference (0..1), compare function, -, -
    uint4 flags;          // x pretransformed, y fog (1 vertex, 2 table), z alpha test, w specular add
    uint4 stage[8];       // x colorop, y alphaop, z colour args (1 | 2 << 8 | 0 << 16), w alpha args
    uint4 stage2[8];      // x result arg, y texture bound, z texcoord index, w -
    float4 stage_const[8];
};

struct Light {
    float4 diffuse, specular, ambient;
    float4 position;      // xyz, w type (1 point, 2 spot, 3 directional)
    float4 direction;     // xyz
    float4 att;           // range, falloff, att0, att1
    float4 att2;          // att2, theta, phi, enabled
};

// Fixed-function vertex state (vertex buffer 17 when there is no vertex shader).
struct FFBlock {
    float4x4 world[4];    // D3D row-major, used as m * v
    float4x4 view;
    float4x4 proj;
    float4 mat_diffuse, mat_ambient, mat_specular, mat_emissive;
    float4 global_ambient;
    float4 misc;          // x specular power
    uint4 flags;          // x lighting, y colour vertex, z blend weights, w normalize normals
    uint4 sources;        // diffuse, specular, ambient, emissive material sources (0 material, 1 colour1, 2 colour2)
    uint4 fogv;           // x vertex fog mode, y the vertex has a specular colour
    uint4 counts;         // x lights
    Light lights[8];
};

inline float rcp1(float x) { return x == 1.0 ? 1.0 : 1.0 / x; }
inline float rsq1(float x) { x = abs(x); return x == 1.0 ? 1.0 : rsqrt(x); }
inline float log1(float x) { x = abs(x); return x == 0.0 ? -INFINITY : log2(x); }

inline float4 vtex(VOut in, uint i) {
    switch (i & 7) {
    case 0: return in.t0; case 1: return in.t1; case 2: return in.t2; case 3: return in.t3;
    case 4: return in.t4; case 5: return in.t5; case 6: return in.t6; default: return in.t7;
    }
}

// D3D9 samples at integer pixel positions; Metal at .5: shift by half a pixel.
inline float4 d3d_pixel_offset(float4 p, constant DrawBlock &d) {
    p.x += p.w / d.viewport.z;
    p.y -= p.w / d.viewport.w;
    return p;
}

inline bool compare(uint f, float a, float b) {
    switch (f) {
    case 1: return false; case 2: return a < b; case 3: return a == b; case 4: return a <= b;
    case 5: return a > b; case 6: return a != b; case 7: return a >= b; default: return true;
    }
}

// Fog and the alpha test, after the pixel colour is known.
inline float4 finish(float4 col, VOut in, constant DrawBlock &d) {
    if (d.flags.y != 0) {
        float f = 1.0;
        if (d.flags.y == 2) {
            float z = in.vdepth;
            uint mode = uint(d.fog.w);
            if (mode == 3) f = (d.fog.y - z) / max(1e-6, d.fog.y - d.fog.x);
            else if (mode == 1) f = exp(-d.fog.z * z);
            else if (mode == 2) f = exp(-(d.fog.z * z) * (d.fog.z * z));
        } else {
            f = in.fog;
        }
        f = saturate(f);
        col.rgb = mix(d.fog_color.rgb, col.rgb, f);
    }
    if (d.flags.z != 0 && !compare(uint(d.alpha.y), round(col.a * 255.0), d.alpha.x)) discard_fragment();
    return col;
}
)MSL";
}

std::string fixed_function() {
    return R"MSL(
// Presenting the frame (and the movie) on the screen.
struct BlitOut { float4 pos [[position]]; float2 uv; };
vertex BlitOut blit_vs(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {
    float2 uv = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    BlitOut o;
    o.pos = float4(mix(rect.x, rect.z, uv.x), mix(rect.y, rect.w, uv.y), 0.0, 1.0);
    o.uv = uv;
    return o;
}
fragment float4 blit_fs(BlitOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return float4(t.sample(s, in.uv).rgb, 1.0);
}

// Clear with rectangles: a quad at depth z.x in the colour c.
struct ClearOut { float4 pos [[position]]; };
vertex ClearOut clear_vs(uint vid [[vertex_id]], constant float4 &z [[buffer(0)]]) {
    ClearOut o;
    o.pos = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0, z.x, 1.0);
    return o;
}
fragment float4 clear_fs(ClearOut in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }

struct FFIn {
    float4 pos [[attribute(0)]];
    float4 post [[attribute(1)]];
    float4 bw [[attribute(2)]];
    float4 bi [[attribute(3)]];
    float4 normal [[attribute(4)]];
    float4 c0 [[attribute(5)]];
    float4 c1 [[attribute(6)]];
    float4 t0 [[attribute(7)]]; float4 t1 [[attribute(8)]]; float4 t2 [[attribute(9)]]; float4 t3 [[attribute(10)]];
    float4 t4 [[attribute(11)]]; float4 t5 [[attribute(12)]]; float4 t6 [[attribute(13)]]; float4 t7 [[attribute(14)]];
    float4 psize [[attribute(15)]];
};

inline float4 ff_source(uint s, float4 mat, float4 c0, float4 c1, bool colourvertex) {
    if (!colourvertex) return mat;
    if (s == 1) return c0;
    if (s == 2) return c1;
    return mat;
}

vertex VOut ff_vertex(FFIn in [[stage_in]], constant FFBlock &f [[buffer(17)]], constant DrawBlock &d [[buffer(18)]]) {
    VOut o;
    o.c0 = in.c0;
    o.c1 = in.c1;
    o.t0 = in.t0; o.t1 = in.t1; o.t2 = in.t2; o.t3 = in.t3; o.t4 = in.t4; o.t5 = in.t5; o.t6 = in.t6; o.t7 = in.t7;
    o.fog = 1.0;
    o.psize = 1.0;
    if (d.flags.x != 0) {  // pre-transformed: screen x, y, z and 1/w
        float w = in.post.w != 0.0 ? 1.0 / in.post.w : 1.0;
        float x = (in.post.x + 0.5 - d.viewport.x) / d.viewport.z * 2.0 - 1.0;
        float y = 1.0 - (in.post.y + 0.5 - d.viewport.y) / d.viewport.w * 2.0;
        o.pos = float4(x * w, y * w, in.post.z * w, w);
        o.vdepth = w;
        o.c0 = saturate(o.c0);
        o.c1 = saturate(o.c1);
        return o;
    }
    float4 p = float4(in.pos.xyz, 1.0);
    float4 wp;
    float3 wn;
    uint blends = f.flags.z;
    if (blends == 0) {
        wp = f.world[0] * p;
        wn = (f.world[0] * float4(in.normal.xyz, 0.0)).xyz;
    } else {
        float wsum = 0.0;
        wp = float4(0.0);
        wn = float3(0.0);
        for (uint i = 0; i < blends; i++) {
            float wi = in.bw[i];
            wsum += wi;
            wp += wi * (f.world[i] * p);
            wn += wi * (f.world[i] * float4(in.normal.xyz, 0.0)).xyz;
        }
        float wl = 1.0 - wsum;
        wp += wl * (f.world[blends] * p);
        wn += wl * (f.world[blends] * float4(in.normal.xyz, 0.0)).xyz;
    }
    float4 vp = f.view * wp;
    float3 vn = (f.view * float4(wn, 0.0)).xyz;
    if (f.flags.w != 0 || true) vn = length(vn) > 0.0 ? normalize(vn) : vn;
    o.pos = d3d_pixel_offset(f.proj * vp, d);
    o.vdepth = vp.z;
    if (f.flags.x != 0) {  // lighting
        bool cv = f.flags.y != 0;
        float4 dif = ff_source(f.sources.x, f.mat_diffuse, in.c0, in.c1, cv);
        float4 amb = ff_source(f.sources.z, f.mat_ambient, in.c0, in.c1, cv);
        float4 emi = ff_source(f.sources.w, f.mat_emissive, in.c0, in.c1, cv);
        float3 ad = float3(0.0), aa = f.global_ambient.rgb;
        for (uint i = 0; i < f.counts.x && i < 8; i++) {
            Light l = f.lights[i];
            float3 L;
            float att = 1.0;
            if (l.position.w == 3.0) {
                L = -(f.view * float4(l.direction.xyz, 0.0)).xyz;
            } else {
                float3 lp = (f.view * float4(l.position.xyz, 1.0)).xyz;
                L = lp - vp.xyz;
                float dist = length(L);
                if (dist > l.att.x) continue;
                att = 1.0 / max(1e-6, l.att.z + l.att.w * dist + l.att2.x * dist * dist);
            }
            L = length(L) > 0.0 ? normalize(L) : L;
            ad += max(0.0, dot(vn, L)) * l.diffuse.rgb * att;
            aa += l.ambient.rgb * att;
        }
        o.c0 = float4(emi.rgb + amb.rgb * aa + dif.rgb * ad, dif.a);
        o.c1 = float4(0.0);
    }
    if (f.fogv.x != 0) {  // vertex fog from eye depth
        float z = abs(vp.z);
        float mode = float(f.fogv.x);
        if (mode == 3.0) o.fog = (d.fog.y - z) / max(1e-6, d.fog.y - d.fog.x);
        else if (mode == 1.0) o.fog = exp(-d.fog.z * z);
        else o.fog = exp(-(d.fog.z * z) * (d.fog.z * z));
    } else if (f.fogv.y != 0) {
        o.fog = in.c1.a;  // no fog mode: the specular alpha is the fog factor
    }
    o.c0 = saturate(o.c0);
    o.c1 = saturate(o.c1);
    return o;
}

inline float4 ff_arg(uint a, float4 diffuse, float4 current, float4 texel, float4 tf, float4 spec, float4 temp, float4 k) {
    float4 v;
    switch (a & 0xF) {
    case 0: v = diffuse; break; case 1: v = current; break; case 2: v = texel; break; case 3: v = tf; break;
    case 4: v = spec; break; case 5: v = temp; break; case 6: v = k; break; default: v = current;
    }
    if ((a & 0x10) != 0) v = 1.0 - v;
    if ((a & 0x20) != 0) v = float4(v.a);
    return v;
}

inline float4 ff_op(uint op, float4 a1, float4 a2, float4 a0, float4 diffuse, float4 texel, float4 tf, float4 current) {
    switch (op) {
    case 2: return a1;
    case 3: return a2;
    case 4: return a1 * a2;
    case 5: return saturate(2.0 * a1 * a2);
    case 6: return saturate(4.0 * a1 * a2);
    case 7: return saturate(a1 + a2);
    case 8: return saturate(a1 + a2 - 0.5);
    case 9: return saturate(2.0 * (a1 + a2 - 0.5));
    case 10: return saturate(a1 - a2);
    case 11: return saturate(a1 + a2 - a1 * a2);
    case 12: return mix(a2, a1, diffuse.a);
    case 13: return mix(a2, a1, texel.a);
    case 14: return mix(a2, a1, tf.a);
    case 15: return saturate(a1 + a2 * (1.0 - texel.a));
    case 16: return mix(a2, a1, current.a);
    case 24: return float4(saturate(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5)));
    case 25: return saturate(a0 + a1 * a2);
    case 26: return mix(a2, a1, a0);
    default: return a1;
    }
}

fragment float4 ff_fragment(VOut in [[stage_in]], array<texture2d<float>, 8> tex [[texture(0)]],
                            array<sampler, 8> smp [[sampler(0)]], constant DrawBlock &d [[buffer(1)]]) {
    float4 diffuse = in.c0, spec = in.c1;
    float4 current = diffuse, temp = float4(0.0);
    for (uint s = 0; s < 8; s++) {
        uint4 st = d.stage[s];
        if (st.x == 1) break;  // COLOROP DISABLE ends the cascade
        float4 texel = float4(0.0, 0.0, 0.0, 1.0);
        if (d.stage2[s].y != 0) texel = tex[s].sample(smp[s], vtex(in, d.stage2[s].z).xy);
        float4 k = d.stage_const[s];
        float4 c1 = ff_arg(st.z & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 c2 = ff_arg((st.z >> 8) & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 c0 = ff_arg((st.z >> 16) & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 a1 = ff_arg(st.w & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 a2 = ff_arg((st.w >> 8) & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 a0 = ff_arg((st.w >> 16) & 0xFF, diffuse, current, texel, d.tfactor, spec, temp, k);
        float4 r;
        r.rgb = ff_op(st.x, c1, c2, c0, diffuse, texel, d.tfactor, current).rgb;
        r.a = st.y == 1 ? current.a : ff_op(st.y, a1, a2, a0, diffuse, texel, d.tfactor, current).a;
        if (d.stage2[s].x == 5) temp = r; else current = r;
    }
    if (d.flags.w != 0) current.rgb = saturate(current.rgb + spec.rgb);
    return finish(current, in, d);
}
)MSL";
}

// ---------------------------------------------------------------------------------------
namespace {
const char *kComp = "xyzw";

std::string fnum(float f) {
    char b[48];
    snprintf(b, sizeof b, "%.9g", double(f));
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    if (s == "inf") return "INFINITY";
    if (s == "-inf") return "(-INFINITY)";
    if (s == "nan") return "NAN";
    return s;
}

struct Translator {
    const Program &p;
    bool pixel;
    std::map<int, std::string> defs;  // constant register -> literal
    std::string error;

    explicit Translator(const Program &prog) : p(prog), pixel(prog.pixel) {
        for (auto &d : p.defs)
            defs[int(d.first)] = "float4(" + fnum(d.second[0]) + ", " + fnum(d.second[1]) + ", " + fnum(d.second[2]) +
                                 ", " + fnum(d.second[3]) + ")";
    }

    std::string base(const Src &s) {
        switch (s.type) {
        case TEMP: return "r" + std::to_string(s.num);
        case INPUT: return pixel ? (s.num == 0 ? "in.c0" : "in.c1") : "in.v" + std::to_string(s.num);
        case CONST: {
            if (!s.relative) {
                auto it = defs.find(s.num);
                if (it != defs.end()) return it->second;
                return std::string(pixel ? "pc[" : "c[") + std::to_string(s.num) + "]";
            }
            return "c[clamp(a0 + " + std::to_string(s.num) + ", 0, 255)]";
        }
        case ADDR_OR_TEX: return pixel ? "t" + std::to_string(s.num) : "float4(float(a0))";
        default:
            error = "source register type " + std::to_string(s.type);
            return "float4(0.0)";
        }
    }

    std::string src(const Src &s) {
        std::string e = base(s);
        if (!(s.swz[0] == 0 && s.swz[1] == 1 && s.swz[2] == 2 && s.swz[3] == 3)) {
            e = "(" + e + ")." + std::string{kComp[s.swz[0]], kComp[s.swz[1]], kComp[s.swz[2]], kComp[s.swz[3]]};
        }
        switch (s.mod) {
        case 0: return e;
        case 1: return "(-" + e + ")";
        case 2: return "(" + e + " - 0.5)";
        case 3: return "(-(" + e + " - 0.5))";
        case 4: return "(2.0 * (" + e + " - 0.5))";
        case 5: return "(-2.0 * (" + e + " - 0.5))";
        case 6: return "(1.0 - " + e + ")";
        case 7: return "(2.0 * " + e + ")";
        case 8: return "(-2.0 * " + e + ")";
        case 11: return "abs(" + e + ")";
        case 12: return "(-abs(" + e + "))";
        default:
            error = "source modifier " + std::to_string(s.mod);
            return e;
        }
    }

    // `expr` is a float4; writes the masked components.
    std::string write(const Dst &d, std::string expr) {
        if (d.shift) {
            float f = d.shift > 0 ? float(1 << d.shift) : 1.0f / float(1 << -d.shift);
            expr = "(" + expr + " * " + fnum(f) + ")";
        }
        if (d.sat) expr = "saturate(" + expr + ")";
        std::string mask;
        for (int i = 0; i < 4; i++)
            if (d.mask & (1 << i)) mask += kComp[i];
        if (mask.empty()) return "";
        std::string target;
        switch (d.type) {
        case TEMP: target = "r" + std::to_string(d.num); break;
        case ADDR_OR_TEX:
            if (!pixel) return "a0 = int(floor((" + expr + ").x));";
            target = "t" + std::to_string(d.num);
            break;
        case RASTOUT:
            if (d.num == 0) target = "o.pos";
            else if (d.num == 1) return "o.fog = (" + expr + ").x;";
            else return "o.psize = (" + expr + ").x;";
            break;
        case ATTROUT: target = d.num == 0 ? "o.c0" : "o.c1"; break;
        case TEXCRDOUT: target = "o.t" + std::to_string(d.num & 7); break;
        case COLOROUT: target = "r0"; break;
        default:
            error = "destination register type " + std::to_string(d.type);
            return "";
        }
        if (mask == "xyzw") return target + " = " + expr + ";";
        return target + "." + mask + " = (" + expr + ")." + mask + ";";
    }

    std::string row(const Src &s, int k) {
        Src r = s;
        r.num = uint16_t(r.num + k);
        return src(r);
    }

    std::string arith(const Ins &in) {
        auto S = [&](int i) { return src(in.s[i]); };
        std::string e;
        switch (in.op) {
        case MOV: case MOVA: e = S(0); break;
        case ADD: e = "(" + S(0) + " + " + S(1) + ")"; break;
        case SUB: e = "(" + S(0) + " - " + S(1) + ")"; break;
        case MUL: e = "(" + S(0) + " * " + S(1) + ")"; break;
        case MAD: e = "(" + S(0) + " * " + S(1) + " + " + S(2) + ")"; break;
        case DP3: e = "float4(dot((" + S(0) + ").xyz, (" + S(1) + ").xyz))"; break;
        case DP4: e = "float4(dot(" + S(0) + ", " + S(1) + "))"; break;
        case MIN: e = "min(" + S(0) + ", " + S(1) + ")"; break;
        case MAX: e = "max(" + S(0) + ", " + S(1) + ")"; break;
        case SLT: e = "select(float4(0.0), float4(1.0), " + S(0) + " < " + S(1) + ")"; break;
        case SGE: e = "select(float4(0.0), float4(1.0), " + S(0) + " >= " + S(1) + ")"; break;
        case RCP: e = "float4(rcp1((" + S(0) + ").w))"; break;
        case RSQ: e = "float4(rsq1((" + S(0) + ").w))"; break;
        case EXP: e = "float4(exp2((" + S(0) + ").w))"; break;
        case LOG: e = "float4(log1((" + S(0) + ").w))"; break;
        case EXPP: {
            std::string x = "(" + S(0) + ").w";
            e = "float4(exp2(floor(" + x + ")), " + x + " - floor(" + x + "), exp2(" + x + "), 1.0)";
            break;
        }
        case LOGP: e = "float4(log1((" + S(0) + ").w))"; break;
        case LIT: {
            std::string a = S(0);
            e = "float4(1.0, max((" + a + ").x, 0.0), (" + a + ").x > 0.0 ? pow(max((" + a + ").y, 0.0), clamp((" + a +
                ").w, -128.0, 128.0)) : 0.0, 1.0)";
            break;
        }
        case DST: e = "float4(1.0, (" + S(0) + ").y * (" + S(1) + ").y, (" + S(0) + ").z, (" + S(1) + ").w)"; break;
        case LRP: e = "mix(" + S(2) + ", " + S(1) + ", " + S(0) + ")"; break;
        case FRC: e = "fract(" + S(0) + ")"; break;
        case POW: e = "float4(pow(abs((" + S(0) + ").w), (" + S(1) + ").w))"; break;
        case ABS: e = "abs(" + S(0) + ")"; break;
        case NRM: e = "float4(normalize((" + S(0) + ").xyz), 1.0)"; break;
        case SINCOS: e = "float4(cos((" + S(0) + ").w), sin((" + S(0) + ").w), 0.0, 0.0)"; break;
        case CND: e = "select(" + S(2) + ", " + S(1) + ", " + S(0) + " > 0.5)"; break;
        case CMP: e = "select(" + S(2) + ", " + S(1) + ", " + S(0) + " >= 0.0)"; break;
        case DP2ADD: e = "float4(dot((" + S(0) + ").xy, (" + S(1) + ").xy) + (" + S(2) + ").x)"; break;
        case M4x4: case M4x3: case M3x4: case M3x3: case M3x2: {
            int rows = in.op == M4x4 || in.op == M3x4 ? 4 : in.op == M3x2 ? 2 : 3;
            bool four = in.op == M4x4 || in.op == M4x3;
            std::string a = S(0);
            std::string parts[4] = {"0.0", "0.0", "0.0", "0.0"};
            for (int k = 0; k < rows; k++)
                parts[k] = four ? "dot(" + a + ", " + row(in.s[1], k) + ")"
                                : "dot((" + a + ").xyz, (" + row(in.s[1], k) + ").xyz)";
            Dst d = in.d;
            d.mask &= uint8_t((1 << rows) - 1);
            return write(d, "float4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")");
        }
        default:
            error = "instruction " + std::to_string(in.op);
            return "";
        }
        return write(in.d, e);
    }
};
}  // namespace

std::string vertex(const Program &p, const std::string &name, std::string *err) {
    Translator t(p);
    std::ostringstream o;
    o << "struct " << name << "_in {\n";
    for (auto &i : p.inputs) o << "    float4 v" << i[0] << " [[attribute(" << i[0] << ")]];\n";
    if (p.inputs.empty()) o << "    float4 v0 [[attribute(0)]];\n";
    o << "};\n";
    o << "vertex VOut " << name << "(" << name << "_in in [[stage_in]], constant float4 *c [[buffer(17)]], "
      << "constant DrawBlock &d [[buffer(18)]]) {\n";
    o << "    VOut o;\n    o.pos = float4(0.0, 0.0, 0.0, 1.0);\n    o.c0 = float4(1.0);\n    o.c1 = float4(0.0);\n";
    o << "    o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = float4(0.0, 0.0, 0.0, 1.0);\n";
    o << "    o.fog = 1.0;\n    o.psize = 1.0;\n    int a0 = 0;\n";
    for (int i = 0; i < 12; i++) o << "    float4 r" << i << " = float4(0.0);\n";
    for (const Ins &in : p.ins) {
        if (in.op == DCL || in.op == DEF || in.op == NOP) continue;
        std::string line = t.arith(in);
        if (!t.error.empty()) break;
        if (!line.empty()) o << "    " << line << "\n";
    }
    o << "    o.vdepth = o.pos.w;\n    o.pos = d3d_pixel_offset(o.pos, d);\n";
    o << "    o.c0 = saturate(o.c0);\n    o.c1 = saturate(o.c1);\n    (void)a0;\n    return o;\n}\n";
    if (!t.error.empty()) {
        if (err) *err = t.error;
        return "";
    }
    return o.str();
}

std::string fragment(const Program &p, const std::string &name, std::string *err) {
    Translator t(p);
    bool ps14 = p.major == 1 && p.minor >= 4;
    std::ostringstream o;
    o << "fragment float4 " << name << "(VOut in [[stage_in]], array<texture2d<float>, 8> tex [[texture(0)]], "
      << "array<sampler, 8> smp [[sampler(0)]], constant float4 *pc [[buffer(0)]], "
      << "constant DrawBlock &d [[buffer(1)]]) {\n";
    for (int i = 0; i < 6; i++) o << "    float4 r" << i << " = float4(0.0);\n";
    for (int i = 0; i < 8; i++) o << "    float4 t" << i << " = float4(0.0);\n";
    for (const Ins &in : p.ins) {
        std::string line;
        switch (in.op) {
        case DCL: case DEF: case NOP: case PHASE: continue;
        case TEX: {
            int stage = in.d.num & 7;
            std::string coord = "in.t" + std::to_string(stage);
            if (ps14 && in.nsrc >= 1)
                coord = in.s[0].type == ADDR_OR_TEX ? "in.t" + std::to_string(in.s[0].num & 7) : t.src(in.s[0]);
            std::string e = "tex[" + std::to_string(stage) + "].sample(smp[" + std::to_string(stage) + "], (" + coord + ").xy)";
            Dst d = in.d;
            if (!ps14) d.type = ADDR_OR_TEX;
            line = t.write(d, e);
            break;
        }
        case TEXCOORD: {
            int r = (ps14 && in.nsrc >= 1 ? in.s[0].num : in.d.num) & 7;
            line = t.write(in.d, "float4(saturate(in.t" + std::to_string(r) + ".xyz), 1.0)");
            break;
        }
        case TEXKILL: {
            std::string v = in.d.type == ADDR_OR_TEX && !ps14 ? "in.t" + std::to_string(in.d.num & 7)
                                                               : "r" + std::to_string(in.d.num);
            line = "if (any((" + v + ").xyz < 0.0)) discard_fragment();";
            break;
        }
        default:
            line = t.arith(in);
        }
        if (!t.error.empty()) break;
        if (!line.empty()) o << "    " << line << "\n";
    }
    o << "    return finish(saturate(r0), in, d);\n}\n";
    if (!t.error.empty()) {
        if (err) *err = t.error;
        return "";
    }
    return o.str();
}

}  // namespace msl
}  // namespace rt
