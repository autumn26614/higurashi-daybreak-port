// Stage shaders: the D3D9 fixed-function state the game uses for stages (port/src/recipe.h),
// pixel for pixel the same rule as the CPU reference renderer (port/src/raster.cpp):
//   colour = texture(uv) * vertex colour   (lighting off, TSS0 MODULATE, colour and alpha)
//   alpha test: discard alpha < 1/255      (ALPHAREF 1, GREATEREQUAL; per pass)
//   fog: linear in view-space depth from env start to end, towards the env colour (per pass)
// Blending, depth write, culling and sampling are pipeline state (StageRenderer.swift).
#include <metal_stdlib>
using namespace metal;

struct StageUniforms {
    float4x4 viewProj;  // D3D row-major V*P loaded as columns: (V*P)^T
    float4x4 view;      // D3D row-major V loaded as columns: V^T
    float4 fogColor;
    float fogStart;
    float fogEnd;
    uint fog;
    uint alphaTest;
    uint mode3;  // render mode 3 vertex colours (port/src/recipe.h), stage passes only
    uint pad0, pad1, pad2;
};

struct StageVertexIn {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float4 color [[attribute(2)]];  // uchar4 normalized
};

struct StageVertexOut {
    float4 position [[position]];
    float2 uv;
    float4 color;  // multiplier
    float3 add;    // render mode 3 addend
    float viewZ;
};

vertex StageVertexOut stage_vertex(StageVertexIn in [[stage_in]], constant StageUniforms& u [[buffer(1)]]) {
    const float4 p = float4(in.position, 1.0);
    StageVertexOut out;
    out.position = u.viewProj * p;  // == p * (V*P) in D3D's row-vector convention
    out.viewZ = (u.view * p).z;
    out.uv = in.uv;
    out.color = in.color;
    out.add = float3(0.0);
    if (u.mode3 != 0) {
        // Game vertex shader 0x6317A4: dark colours double, bright ones add above 0.5.
        const float m = max(in.color.r, max(in.color.g, in.color.b));
        if (m < 0.5) {
            out.color = float4(in.color.rgb * 2.0, in.color.a);
        } else {
            out.color = float4(1.0);
            out.add = in.color.rgb - 0.5;
        }
    }
    return out;
}

fragment float4 stage_fragment(StageVertexOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                               sampler smp [[sampler(0)]], constant StageUniforms& u [[buffer(1)]]) {
    float4 c = tex.sample(smp, in.uv) * in.color;
    c.rgb = min(c.rgb + in.add, 1.0);
    if (u.alphaTest != 0 && c.a < 1.0 / 255.0) discard_fragment();
    if (u.fog != 0 && u.fogEnd > u.fogStart) {
        const float f = clamp((u.fogEnd - in.viewZ) / (u.fogEnd - u.fogStart), 0.0, 1.0);
        c.rgb = mix(u.fogColor.rgb, c.rgb, f);
    }
    return c;
}

// Character outline: the game's outline pass (vertex shader 0x62FD94, vs_1_1). The normal
// goes through the same view-projection as the position, is normalised as a 4-vector, and
// pushes the vertex by (0.00275, 0.00366667) * w in clip space: about 1.1 px at 800x600.
// Its colour register (c5) is never set by the game's code or effect states that were
// found, so it is taken as black (inferred). Drawn with back faces only.
struct OutlineVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(3)]];
};

vertex float4 outline_vertex(OutlineVertexIn in [[stage_in]], constant StageUniforms& u [[buffer(1)]]) {
    float4 p = u.viewProj * float4(in.position, 1.0);
    float4 n = u.viewProj * float4(in.normal, 0.0);
    const float len = length(n);
    if (len > 0) n /= len;
    p.xy += n.xy * p.w * float2(0.00275, 0.00366667);
    p.z += p.w * 1e-6;
    return p;
}

fragment float4 outline_fragment() { return float4(0.0, 0.0, 0.0, 1.0); }
