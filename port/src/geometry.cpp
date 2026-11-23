#include "geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace db {

namespace {

using Mat4 = std::array<double, 16>;

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
// D3D row-vector convention: combined = local * parent.
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    return r;
}

// A count stored as a double in the .x number list, clamped to [0, limit].
size_t count_of(double v, size_t limit) {
    if (!(v >= 0)) return 0;
    if (v >= double(limit)) return limit;
    return size_t(v);
}

float unit(double v) { return float(std::min(1.0, std::max(0.0, v))); }

GMaterial material(const Source& a, const XObject& m, const std::string& dir) {
    GMaterial g;
    const auto& n = m.nums;  // faceColor RGBA, power, specular RGB, emissive RGB
    if (n.size() >= 4)
        for (int i = 0; i < 4; i++) g.diffuse[i] = float(n[size_t(i)]);
    if (n.size() >= 5) g.power = float(n[4]);
    if (n.size() >= 8)
        for (int i = 0; i < 3; i++) g.specular[i] = float(n[size_t(5 + i)]);
    if (n.size() >= 11)
        for (int i = 0; i < 3; i++) g.emissive[i] = float(n[size_t(8 + i)]);
    for (const auto& k : m.kids)
        if (k.type == "TextureFilename" && !k.strs.empty()) g.texture = k.strs[0];
    if (!g.texture.empty()) g.tex = resolve_texture(a, dir, g.texture);
    return g;
}

struct Builder {
    const Source& a;
    const std::string& dir;
    int slot;
    StageGeometry& g;
    std::map<std::string, const XObject*> named;
    int mesh_index = 0;

    void mesh(const XObject& m, const Mat4& world) {
        const int this_mesh = mesh_index++;
        const auto& n = m.nums;
        const size_t nv = n.empty() ? 0 : count_of(n[0], (n.size() - 1) / 3);
        // World-space positions (identity for every shipped stage file).
        std::vector<float> pos(nv * 3);
        for (size_t v = 0; v < nv; v++) {
            const double p[3] = {n[1 + v * 3], n[2 + v * 3], n[3 + v * 3]};
            for (int c = 0; c < 3; c++)
                pos[v * 3 + size_t(c)] =
                    float(p[0] * world[0 * 4 + c] + p[1] * world[1 * 4 + c] + p[2] * world[2 * 4 + c] + world[12 + c]);
        }
        // Faces: count, then per face its vertex count and indices; fan-triangulated.
        std::vector<std::vector<uint32_t>> face_tris;
        size_t q = 1 + nv * 3;
        const size_t nf = q < n.size() ? count_of(n[q], n.size() - q - 1) : 0;
        q++;
        for (size_t f = 0; f < nf && q < n.size(); f++) {
            const size_t cnt = count_of(n[q], n.size() - q - 1);
            std::vector<uint32_t> tris;
            bool ok = true;
            std::vector<uint32_t> idx(cnt);
            for (size_t k = 0; k < cnt; k++) {
                const size_t i = count_of(n[q + 1 + k], SIZE_MAX);
                if (i >= nv) ok = false;
                idx[k] = uint32_t(std::min(i, nv ? nv - 1 : 0));
            }
            if (ok)
                for (size_t k = 2; k < cnt; k++) tris.insert(tris.end(), {idx[0], idx[k - 1], idx[k]});
            face_tris.push_back(std::move(tris));
            q += cnt + 1;
        }

        std::vector<float> uv(nv * 2, 0.0f);
        std::vector<uint8_t> rgba(nv * 4, 255);
        bool has_colors = false;
        std::vector<GMaterial> mats;
        std::vector<uint32_t> face_mat(face_tris.size(), 0);
        for (const auto& k : m.kids) {
            const auto& kn = k.nums;
            if (k.type == "MeshTextureCoords" && !kn.empty()) {
                const size_t c = std::min(count_of(kn[0], (kn.size() - 1) / 2), nv);
                for (size_t v = 0; v < c; v++) {
                    uv[v * 2] = float(kn[1 + v * 2]);
                    uv[v * 2 + 1] = float(kn[2 + v * 2]);
                }
            } else if (k.type == "MeshVertexColors" && !kn.empty()) {
                // count, then (vertex index, r, g, b, a) per entry
                const size_t c = count_of(kn[0], (kn.size() - 1) / 5);
                for (size_t e = 0; e < c; e++) {
                    const size_t v = count_of(kn[1 + e * 5], SIZE_MAX);
                    if (v >= nv) continue;
                    for (int ch = 0; ch < 4; ch++)
                        rgba[v * 4 + size_t(ch)] = uint8_t(std::lround(unit(kn[2 + e * 5 + size_t(ch)]) * 255.0));
                }
                has_colors = c > 0;
            } else if (k.type == "MeshMaterialList" && kn.size() >= 2) {
                const size_t nidx = count_of(kn[1], kn.size() - 2);
                // Fewer indices than faces: the last one applies to the rest (.x spec).
                for (size_t f = 0; f < face_mat.size() && nidx > 0; f++)
                    face_mat[f] = uint32_t(count_of(kn[2 + std::min(f, nidx - 1)], UINT32_MAX));
                for (const auto& mk : k.kids)
                    if (mk.type == "Material") mats.push_back(material(a, mk, dir));
                for (const auto& r : k.refs) {
                    auto it = named.find(r);
                    mats.push_back(it != named.end() ? material(a, *it->second, dir) : GMaterial{});
                }
            }
        }
        if (mats.empty()) mats.push_back(GMaterial{});  // CreateMeshContainer: one white default

        // One batch per material, with only the vertices it uses (re-indexed).
        for (size_t mi = 0; mi < mats.size(); mi++) {
            GBatch b;
            b.slot = slot;
            b.mesh = this_mesh;
            b.material = int(mi);
            b.mat = mats[mi];
            b.has_colors = has_colors;
            std::unordered_map<uint32_t, uint32_t> remap;
            for (size_t f = 0; f < face_tris.size(); f++) {
                if (std::min<size_t>(face_mat[f], mats.size() - 1) != mi) continue;
                for (uint32_t v : face_tris[f]) {
                    auto it = remap.find(v);
                    if (it == remap.end()) {
                        it = remap.emplace(v, uint32_t(remap.size())).first;
                        b.positions.insert(b.positions.end(), &pos[v * 3], &pos[v * 3] + 3);
                        b.uvs.insert(b.uvs.end(), &uv[v * 2], &uv[v * 2] + 2);
                        b.colors.insert(b.colors.end(), &rgba[v * 4], &rgba[v * 4] + 4);
                    }
                    b.indices.push_back(it->second);
                }
            }
            if (!b.indices.empty()) g.batches.push_back(std::move(b));
        }
    }

    void frame(const XObject& f, const Mat4& parent) {
        Mat4 local = identity();
        for (const auto& k : f.kids)
            if (k.type == "FrameTransformMatrix" && k.nums.size() >= 16)
                std::copy(k.nums.begin(), k.nums.begin() + 16, local.begin());
        const Mat4 world = mul(local, parent);
        for (const auto& k : f.kids) {
            if (k.type == "Frame") frame(k, world);
            if (k.type == "Mesh") mesh(k, world);
        }
    }
};

}  // namespace

size_t StageGeometry::triangles() const {
    size_t t = 0;
    for (const auto& b : batches) t += b.indices.size() / 3;
    return t;
}

bool parse_env(const std::vector<uint8_t>& d, StageEnv& env) {
    if (d.size() < 84) return false;
    int32_t v[21];
    for (int i = 0; i < 21; i++) v[i] = int32_t(load_le32(&d[size_t(i) * 4]));
    env.present = true;
    env.fog_start = v[0];
    env.fog_end = v[1];
    for (int i = 0; i < 3; i++) env.fog_rgb[i] = uint8_t(v[2 + i] & 255);
    for (int i = 0; i < 4; i++) env.params[i] = v[5 + i];
    for (int p = 0; p < 4; p++)
        for (int c = 0; c < 3; c++) env.spawn[p][c] = float(v[9 + p * 3 + c]);
    return true;
}

void append_x_batches(const Source& a, const XFile& x, const std::string& dir, int slot, StageGeometry& g) {
    Builder b{a, dir, slot, g, {}, 0};
    for (const auto& r : x.roots)
        if (r.type == "Material" && !r.name.empty()) b.named[r.name] = &r;
    for (const auto& r : x.roots) {
        if (r.type == "Frame") b.frame(r, identity());
        if (r.type == "Mesh") b.mesh(r, identity());
    }
}

StageGeometry load_stage_geometry(const Source& a, int stage, int variant) {
    StageGeometry g;
    g.stage = stage;
    g.variant = variant ? 1 : 0;
    char dirbuf[64];
    snprintf(dirbuf, sizeof dirbuf, "data/stage%02d", stage);
    g.dir = dirbuf;
    if (const Entry* e = a.find(g.dir + "/env.dat")) {
        std::vector<uint8_t> d;
        std::string err;
        if (!a.read(*e, d, &err) || !parse_env(d, g.env)) g.errors.push_back("env.dat: " + (err.empty() ? "too short" : err));
    }
    for (int slot = 0; slot < 8; slot++) {
        char nb[32];
        snprintf(nb, sizeof nb, g.variant ? "%02db.x" : "%02d.x", slot);
        XFile x;
        std::string err;
        if (!load_x(a, g.dir + "/" + nb, x, &err)) {
            if (err != "missing") g.errors.push_back(std::string(nb) + ": " + err);
            continue;
        }
        append_x_batches(a, x, g.dir + "/", slot, g);
    }
    // Bounds without the sky dome, unless the sky is all there is.
    for (int with_sky = 0; with_sky < 2 && g.bounds.empty(); with_sky++)
        for (const auto& b : g.batches) {
            if (b.slot == 0 && !with_sky) continue;
            for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
                const double p[3] = {b.positions[i], b.positions[i + 1], b.positions[i + 2]};
                g.bounds.add(p);
            }
        }
    return g;
}

}  // namespace db
