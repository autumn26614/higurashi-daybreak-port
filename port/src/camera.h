// Camera maths in the game's own convention: Direct3D 9, left-handed, row vectors
// (v' = v * M), D3DX formulas (D3DXMatrixLookAtLH / D3DXMatrixPerspectiveFovLH, the
// call at 0x5D6500 uses fov pi/4). Metal's clip space is the same as D3D's (x right,
// y up, z 0..1 into the screen), so these matrices feed the Metal renderer unchanged:
// a row-major array loaded as an MSL float4x4 is its transpose, and M^T * v == v * M.
#pragma once
#include <algorithm>
#include <cmath>

#include "geometry.h"

namespace db {

struct Mat4f {
    float m[16];  // row-major
};

inline Mat4f mul(const Mat4f& a, const Mat4f& b) {
    Mat4f r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

// out = in * M (row vector).
inline void transform(const Mat4f& M, const float in[4], float out[4]) {
    for (int j = 0; j < 4; j++)
        out[j] = in[0] * M.m[0 * 4 + j] + in[1] * M.m[1 * 4 + j] + in[2] * M.m[2 * 4 + j] + in[3] * M.m[3 * 4 + j];
}

inline Mat4f look_at_lh(const float eye[3], const float at[3]) {
    auto norm = [](float v[3]) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 0) v[0] /= l, v[1] /= l, v[2] /= l;
    };
    float z[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    norm(z);
    const float up[3] = {0, 1, 0};
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    norm(x);
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    auto dot = [&](const float* a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
    return {{x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x), -dot(y), -dot(z), 1}};
}

inline Mat4f perspective_lh(float fov_y, float aspect, float zn, float zf) {
    const float ys = 1.0f / std::tan(fov_y / 2), xs = ys / aspect, q = zf / (zf - zn);
    return {{xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, q, 1, 0, 0, -zn * q, 0}};
}

struct Camera {
    float eye[3], target[3];
    float fov_y = 0.78539816f;  // pi/4, as at 0x5D6500
    float z_near = 50, z_far = 200000;  // the sky dome reaches ~54 000 from the centre
};

// A battle-like view: from behind the arena's near edge (env.dat ints 5-8, inferred to be
// the arena rectangle x0, z0, x1, z1: every stage's spawn points lie inside it), raised,
// looking at its centre at spawn height. Falls back to the mesh bounds.
inline Camera default_camera(const StageGeometry& g) {
    Camera c;
    const int32_t* p = g.env.params;
    float x0, z0, x1, z1, y = 0;
    if (g.env.present && p[0] < p[2] && p[1] < p[3]) {
        x0 = float(p[0]), z0 = float(p[1]), x1 = float(p[2]), z1 = float(p[3]);
        for (int i = 0; i < 4; i++) y += g.env.spawn[i][1] / 4;
    } else {
        x0 = float(g.bounds.lo[0]), z0 = float(g.bounds.lo[2]), x1 = float(g.bounds.hi[0]), z1 = float(g.bounds.hi[2]);
        y = float(g.bounds.lo[1] + g.bounds.hi[1]) / 2 - 150;
    }
    const float size = std::max({x1 - x0, z1 - z0, float(g.bounds.hi[1] - g.bounds.lo[1]), 1.0f});
    c.target[0] = (x0 + x1) / 2, c.target[1] = y + 150, c.target[2] = (z0 + z1) / 2;
    // Steep enough to clear trees and walls around the arena (stage 03 is ringed by forest).
    c.eye[0] = c.target[0], c.eye[1] = c.target[1] + 0.6f * size, c.eye[2] = c.target[2] - 0.5f * size;
    return c;
}

// Full-body view of a posed character: from its front (-z side), slightly above the
// middle of its bounds.
inline Camera character_camera(const StageGeometry& g) {
    Camera c;
    const Box& b = g.bounds;
    const float h = std::max(float(b.hi[1] - b.lo[1]), 1.0f);
    for (int k = 0; k < 3; k++) c.target[k] = float(b.lo[k] + b.hi[k]) / 2;
    c.eye[0] = c.target[0], c.eye[1] = c.target[1] + 0.15f * h, c.eye[2] = c.target[2] - 2.6f * h;
    c.z_near = 5;
    c.z_far = 20000;
    return c;
}

}  // namespace db
