#include "raster.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "cnv.h"
#include "recipe.h"

namespace db {

namespace {

struct V4 {
    float x, y, z, w;
};

struct Tex {
    int w = 1, h = 1;
    std::vector<uint8_t> rgba{255, 255, 255, 255};
};

struct Vert {
    V4 clip;
    float u, v;
    float c[4];
    float add[3];  // render mode 3: added after the texture multiply
    float vz;  // view-space depth, for fog
};

struct Target {
    int w, h;
    std::vector<float> color;  // RGB, 0..1
    std::vector<float> depth;
};

void sample(const Tex& t, float u, float v, bool point, float out[4]) {
    // D3D texel centres at (i + 0.5) / size; wrap addressing.
    auto texel = [&](int x, int y, int c) {
        x = ((x % t.w) + t.w) % t.w;
        y = ((y % t.h) + t.h) % t.h;
        return t.rgba[size_t((y * t.w + x) * 4 + c)] / 255.0f;
    };
    const float fx = u * t.w - 0.5f, fy = v * t.h - 0.5f;
    if (point) {
        const int x = int(std::floor(fx + 0.5f)), y = int(std::floor(fy + 0.5f));
        for (int c = 0; c < 4; c++) out[c] = texel(x, y, c);
        return;
    }
    const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const float ax = fx - x0, ay = fy - y0;
    for (int c = 0; c < 4; c++)
        out[c] = (texel(x0, y0, c) * (1 - ax) + texel(x0 + 1, y0, c) * ax) * (1 - ay) +
                 (texel(x0, y0 + 1, c) * (1 - ax) + texel(x0 + 1, y0 + 1, c) * ax) * ay;
}

Vert lerp(const Vert& a, const Vert& b, float t) {
    Vert r;
    r.clip = {a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
              a.clip.z + (b.clip.z - a.clip.z) * t, a.clip.w + (b.clip.w - a.clip.w) * t};
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    for (int c = 0; c < 4; c++) r.c[c] = a.c[c] + (b.c[c] - a.c[c]) * t;
    for (int c = 0; c < 3; c++) r.add[c] = a.add[c] + (b.add[c] - a.add[c]) * t;
    r.vz = a.vz + (b.vz - a.vz) * t;
    return r;
}

// Clip against the near plane (z >= 0 in D3D / Metal clip space).
std::vector<Vert> clip_near(const std::vector<Vert>& in) {
    std::vector<Vert> out;
    for (size_t i = 0; i < in.size(); i++) {
        const Vert& a = in[i];
        const Vert& b = in[(i + 1) % in.size()];
        const bool ia = a.clip.z >= 0, ib = b.clip.z >= 0;
        if (ia) out.push_back(a);
        if (ia != ib) out.push_back(lerp(a, b, a.clip.z / (a.clip.z - b.clip.z)));
    }
    return out;
}

struct Raster {
    Target& rt;
    const Pass& pass;
    const Tex& tex;
    const StageEnv& env;
    long pixels = 0;

    void triangle(const Vert& a, const Vert& b, const Vert& c) {
        std::vector<Vert> poly = clip_near({a, b, c});
        for (size_t i = 1; i + 1 < poly.size(); i++) screen_tri(poly[0], poly[i], poly[i + 1]);
    }

    void screen_tri(const Vert& a, const Vert& b, const Vert& c) {
        const Vert* v[3] = {&a, &b, &c};
        float sx[3], sy[3], sz[3], iw[3];
        for (int i = 0; i < 3; i++) {
            iw[i] = 1.0f / v[i]->clip.w;
            sx[i] = (v[i]->clip.x * iw[i] * 0.5f + 0.5f) * rt.w;
            sy[i] = (0.5f - v[i]->clip.y * iw[i] * 0.5f) * rt.h;  // y down in the image
            sz[i] = v[i]->clip.z * iw[i];
        }
        // Signed area in image space (y down): positive = clockwise as seen on screen.
        const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (area <= 0) return;  // CULLMODE CCW: counter-clockwise faces are culled
        const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]}))));
        const int x1 = std::min(rt.w - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]}))));
        const int y1 = std::min(rt.h - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                const float px = x + 0.5f, py = y + 0.5f;
                float l[3];
                for (int i = 0; i < 3; i++) {
                    const int j = (i + 1) % 3, k = (i + 2) % 3;
                    l[i] = ((sx[k] - sx[j]) * (py - sy[j]) - (sy[k] - sy[j]) * (px - sx[j])) / area;
                }
                if (l[0] < 0 || l[1] < 0 || l[2] < 0) continue;
                const float z = l[0] * sz[0] + l[1] * sz[1] + l[2] * sz[2];
                if (z < 0 || z > 1) continue;
                const size_t idx = size_t(y * rt.w + x);
                if (z > rt.depth[idx]) continue;  // LESSEQUAL
                // Perspective-correct attributes.
                const float pw[3] = {l[0] * iw[0], l[1] * iw[1], l[2] * iw[2]};
                const float sw = pw[0] + pw[1] + pw[2];
                auto attr = [&](float Vert::*m) { return (pw[0] * (a.*m) + pw[1] * (b.*m) + pw[2] * (c.*m)) / sw; };
                float col[4];
                for (int ch = 0; ch < 4; ch++)
                    col[ch] = (pw[0] * a.c[ch] + pw[1] * b.c[ch] + pw[2] * c.c[ch]) / sw;
                float t[4];
                sample(tex, attr(&Vert::u), attr(&Vert::v), pass.point_sampling, t);
                for (int ch = 0; ch < 4; ch++) col[ch] *= t[ch];
                for (int ch = 0; ch < 3; ch++)
                    col[ch] = std::min(1.0f, col[ch] + (pw[0] * a.add[ch] + pw[1] * b.add[ch] + pw[2] * c.add[ch]) / sw);
                if (pass.alpha_test && col[3] < 1.0f / 255.0f) continue;
                if (pass.fog && env.fog_end > env.fog_start) {
                    const float vz = attr(&Vert::vz);
                    const float f = std::min(1.0f, std::max(0.0f, (env.fog_end - vz) / float(env.fog_end - env.fog_start)));
                    for (int ch = 0; ch < 3; ch++) col[ch] = col[ch] * f + env.fog_rgb[ch] / 255.0f * (1 - f);
                }
                float* dst = &rt.color[idx * 3];
                for (int ch = 0; ch < 3; ch++) {
                    switch (pass.blend) {
                    case Blend::Alpha: dst[ch] = col[ch] * col[3] + dst[ch] * (1 - col[3]); break;
                    case Blend::Multiply: dst[ch] = dst[ch] * col[ch]; break;
                    case Blend::Additive: dst[ch] = std::min(1.0f, dst[ch] + col[ch] * col[3]); break;
                    }
                }
                if (pass.z_write) rt.depth[idx] = z;
                pixels++;
            }
    }
};

}  // namespace

void mode3_colors(float c[4], float add[3]) {
    const float m = std::max(c[0], std::max(c[1], c[2]));
    if (m < 0.5f) {
        for (int k = 0; k < 3; k++) c[k] *= 2, add[k] = 0;
    } else {
        for (int k = 0; k < 3; k++) add[k] = c[k] - 0.5f, c[k] = 1;
        c[3] = 1;
    }
}

long render_reference(const Source& a, const StageGeometry& g, const Camera& cam, int W, int H,
                      std::vector<uint8_t>& out) {
    out.assign(size_t(W) * size_t(H) * 4, 255);
    if (W <= 0 || H <= 0) return 0;
    const Mat4f view = look_at_lh(cam.eye, cam.target);
    const Mat4f proj = perspective_lh(cam.fov_y, float(W) / float(H), cam.z_near, cam.z_far);

    std::map<std::string, Tex> textures;
    auto texture = [&](const GBatch& b) -> const Tex& {
        const std::string& name = b.mat.tex.archive_name;
        auto it = textures.find(name);
        if (it != textures.end()) return it->second;
        Tex t;
        if (b.mat.tex.found) {
            const Entry* e = a.find(name);
            std::vector<uint8_t> d;
            Image img;
            std::string err;
            if (e && a.read(*e, d, &err) && decode_image(d, img, &err)) {
                t.w = int(img.width);
                t.h = int(img.height);
                t.rgba = img.rgba;
            }
        }
        return textures.emplace(name, std::move(t)).first->second;
    };

    Target rt{W, H, std::vector<float>(size_t(W) * size_t(H) * 3), std::vector<float>(size_t(W) * size_t(H), 1.0f)};
    for (size_t i = 0; i < size_t(W) * size_t(H); i++)
        for (int c = 0; c < 3; c++) rt.color[i * 3 + size_t(c)] = g.env.fog_rgb[c] / 255.0f;

    long drawn = 0;
    for (int p = 0; p < kStagePassCount; p++) {
        for (const GBatch& b : g.batches) {
            if (b.slot != kStagePasses[p].slot) continue;
            const Pass pass = subset_pass(kStagePasses[p], b.mat.power);
            Raster r{rt, pass, texture(b), g.env};
            std::vector<Vert> vs(b.positions.size() / 3);
            for (size_t i = 0; i < vs.size(); i++) {
                const float w[4] = {b.positions[i * 3], b.positions[i * 3 + 1], b.positions[i * 3 + 2], 1};
                float vv[4], cc[4];
                transform(view, w, vv);
                transform(proj, vv, cc);
                vs[i].clip = {cc[0], cc[1], cc[2], cc[3]};
                vs[i].vz = vv[2];
                vs[i].u = b.uvs[i * 2];
                vs[i].v = b.uvs[i * 2 + 1];
                for (int c = 0; c < 4; c++) vs[i].c[c] = b.colors[i * 4 + size_t(c)] / 255.0f;
                for (int c = 0; c < 3; c++) vs[i].add[c] = 0;
                if (g.mode3 && pass.mode3) mode3_colors(vs[i].c, vs[i].add);
            }
            for (size_t i = 0; i + 2 < b.indices.size(); i += 3)
                r.triangle(vs[b.indices[i]], vs[b.indices[i + 1]], vs[b.indices[i + 2]]);
            drawn += r.pixels;
        }
    }
    for (size_t i = 0; i < size_t(W) * size_t(H); i++)
        for (int c = 0; c < 3; c++)
            out[i * 4 + size_t(c)] = uint8_t(std::lround(std::min(1.0f, std::max(0.0f, rt.color[i * 3 + size_t(c)])) * 255));
    return drawn;
}

}  // namespace db
