#include "character.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>

#include "stage.h"

namespace db {

namespace {

using M = std::array<double, 16>;

M ident() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

M mul(const M& a, const M& b) {
    M r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[size_t(i * 4 + k)] * b[size_t(k * 4 + j)];
            r[size_t(i * 4 + j)] = s;
        }
    return r;
}

M from(const std::vector<double>& n, size_t at) {
    M m = ident();
    if (at + 16 <= n.size())
        for (size_t i = 0; i < 16; i++) m[i] = n[at + i];
    return m;
}

size_t count_of(double v, size_t limit) {
    if (!(v >= 0)) return 0;
    if (v >= double(limit)) return limit;
    return size_t(v);
}

// Quaternion (w, x, y, z) -> D3D rotation matrix (row vectors), as D3DXMatrixRotationQuaternion.
M quat_matrix(double w, double x, double y, double z) {
    M m = ident();
    m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + z * w), m[2] = 2 * (x * z - y * w);
    m[4] = 2 * (x * y - z * w), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + x * w);
    m[8] = 2 * (x * z + y * w), m[9] = 2 * (y * z - x * w), m[10] = 1 - 2 * (x * x + y * y);
    return m;
}

struct Loader {
    const Source& a;
    std::string dir;
    Character& c;
    std::map<std::string, int> by_name;
    std::map<std::string, const XObject*> named_materials;

    GMaterial material(const XObject& m) {
        GMaterial g;
        const auto& n = m.nums;
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

    void mesh(const XObject& m, int node) {
        CharMesh cm;
        cm.node = node;
        const auto& n = m.nums;
        const size_t nv = n.empty() ? 0 : count_of(n[0], (n.size() - 1) / 3);
        cm.positions.assign(n.begin() + 1, n.begin() + 1 + long(nv * 3));
        size_t q = 1 + nv * 3;
        const size_t nf = q < n.size() ? count_of(n[q], n.size() - q - 1) : 0;
        q++;
        std::vector<std::vector<uint32_t>> faces;
        for (size_t f = 0; f < nf && q < n.size(); f++) {
            const size_t cnt = count_of(n[q], n.size() - q - 1);
            std::vector<uint32_t> idx;
            for (size_t k = 0; k < cnt; k++) idx.push_back(uint32_t(std::min(count_of(n[q + 1 + k], SIZE_MAX), nv ? nv - 1 : 0)));
            faces.push_back(std::move(idx));
            q += cnt + 1;
        }
        cm.normals.assign(nv * 3, 0.0f);
        cm.uvs.assign(nv * 2, 0.0f);
        std::vector<uint32_t> face_mat(faces.size(), 0);
        bool have_normals = false;
        for (const auto& k : m.kids) {
            const auto& kn = k.nums;
            if (k.type == "MeshNormals" && !kn.empty()) {
                // normals, then per face the normal indices of its corners
                const size_t nn = count_of(kn[0], (kn.size() - 1) / 3);
                size_t p = 1 + nn * 3;
                const size_t nfn = p < kn.size() ? count_of(kn[p], kn.size() - p - 1) : 0;
                p++;
                for (size_t f = 0; f < nfn && f < faces.size() && p < kn.size(); f++) {
                    const size_t cnt = count_of(kn[p], kn.size() - p - 1);
                    for (size_t k2 = 0; k2 < cnt && k2 < faces[f].size(); k2++) {
                        const size_t ni = count_of(kn[p + 1 + k2], nn ? nn - 1 : 0);
                        const uint32_t v = faces[f][k2];
                        for (int c3 = 0; c3 < 3; c3++) cm.normals[v * 3 + size_t(c3)] += float(kn[1 + ni * 3 + size_t(c3)]);
                    }
                    p += cnt + 1;
                }
                have_normals = nn > 0;
            } else if (k.type == "MeshTextureCoords" && !kn.empty()) {
                const size_t cnt = std::min(count_of(kn[0], (kn.size() - 1) / 2), nv);
                for (size_t v = 0; v < cnt * 2; v++) cm.uvs[v] = float(kn[1 + v]);
            } else if (k.type == "MeshMaterialList" && kn.size() >= 2) {
                const size_t nidx = count_of(kn[1], kn.size() - 2);
                for (size_t f = 0; f < face_mat.size() && nidx > 0; f++)
                    face_mat[f] = uint32_t(count_of(kn[2 + std::min(f, nidx - 1)], UINT32_MAX));
                for (const auto& mk : k.kids)
                    if (mk.type == "Material") cm.materials.push_back(material(mk));
                for (const auto& r : k.refs) {
                    auto it = named_materials.find(r);
                    cm.materials.push_back(it != named_materials.end() ? material(*it->second) : GMaterial{});
                }
            } else if (k.type == "SkinWeights" && !k.strs.empty() && !kn.empty()) {
                CharBoneWeights w;
                auto it = by_name.find(k.strs[0]);
                w.node = it == by_name.end() ? -1 : it->second;
                const size_t cnt = count_of(kn[0], (kn.size() - 1 - 16) / 2);
                for (size_t i = 0; i < cnt; i++) {
                    w.vertices.push_back(uint32_t(std::min(count_of(kn[1 + i], SIZE_MAX), nv ? nv - 1 : 0)));
                    w.weights.push_back(float(kn[1 + cnt + i]));
                }
                w.offset = from(kn, 1 + cnt * 2);
                if (w.node < 0) c.errors.push_back("skin bone not found: " + k.strs[0]);
                cm.skin.push_back(std::move(w));
            }
        }
        if (!have_normals) {
            // Face normals accumulated per vertex (D3DXComputeNormals when the FVF lacks them).
            for (const auto& f : faces)
                for (size_t k = 2; k < f.size(); k++) {
                    const float* p0 = &cm.positions[f[0] * 3];
                    const float* p1 = &cm.positions[f[k - 1] * 3];
                    const float* p2 = &cm.positions[f[k] * 3];
                    const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
                    const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                    const float nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                          e1[0] * e2[1] - e1[1] * e2[0]};
                    for (uint32_t v : {f[0], f[k - 1], f[k]})
                        for (int c3 = 0; c3 < 3; c3++) cm.normals[v * 3 + size_t(c3)] += nrm[c3];
                }
        }
        for (size_t v = 0; v < nv; v++) {
            float* nn = &cm.normals[v * 3];
            const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l > 0) nn[0] /= l, nn[1] /= l, nn[2] /= l;
        }
        if (cm.materials.empty()) cm.materials.push_back(GMaterial{});
        for (size_t f = 0; f < faces.size(); f++)
            for (size_t k = 2; k < faces[f].size(); k++) {
                cm.tri_vertices.insert(cm.tri_vertices.end(), {faces[f][0], faces[f][k - 1], faces[f][k]});
                cm.tri_material.push_back(std::min<uint32_t>(face_mat[f], uint32_t(cm.materials.size() - 1)));
            }
        c.meshes.push_back(std::move(cm));
    }

    // Two passes: frames first (so SkinWeights can name any bone), then meshes.
    void frames(const XObject& f, int parent) {
        CharNode node;
        node.name = f.name;
        node.parent = parent;
        node.bind = ident();
        for (const auto& k : f.kids)
            if (k.type == "FrameTransformMatrix") node.bind = from(k.nums, 0);
        const int id = int(c.nodes.size());
        c.nodes.push_back(node);
        if (!f.name.empty() && !by_name.count(f.name)) by_name[f.name] = id;
        frame_ids[&f] = id;
        for (const auto& k : f.kids)
            if (k.type == "Frame") frames(k, id);
    }
    void meshes(const XObject& f) {
        const int id = frame_ids[&f];
        for (const auto& k : f.kids) {
            if (k.type == "Mesh") mesh(k, id);
            if (k.type == "Frame") meshes(k);
        }
    }
    std::map<const XObject*, int> frame_ids;

    void anim_set(const XObject& s) {
        CharAnim an;
        an.name = s.name;
        for (const auto& anim : s.kids) {
            if (anim.type != "Animation") continue;
            CharAnimTrack t;
            std::string target = anim.refs.empty() ? "" : anim.refs[0];
            for (const auto& k : anim.kids)
                if (k.type == "Frame") target = k.name;
            auto it = by_name.find(target);
            if (it == by_name.end()) continue;
            t.node = it->second;
            // Keys of one type; several AnimationKey objects combine as S * R * T.
            std::map<double, M> rot, scl, pos, mat;
            for (const auto& key : anim.kids) {
                if (key.type != "AnimationKey" || key.nums.size() < 2) continue;
                const int type = int(key.nums[0]);
                const size_t nk = count_of(key.nums[1], key.nums.size());
                size_t p = 2;
                for (size_t i = 0; i < nk && p + 1 < key.nums.size(); i++) {
                    const double time = key.nums[p];
                    const size_t nvals = count_of(key.nums[p + 1], key.nums.size() - p - 2);
                    const size_t v = p + 2;
                    if (type == 4 && nvals == 16) mat[time] = from(key.nums, v);
                    if (type == 0 && nvals == 4)
                        rot[time] = quat_matrix(key.nums[v], key.nums[v + 1], key.nums[v + 2], key.nums[v + 3]);
                    if (type == 1 && nvals == 3) {
                        M m = ident();
                        m[0] = key.nums[v], m[5] = key.nums[v + 1], m[10] = key.nums[v + 2];
                        scl[time] = m;
                    }
                    if (type == 2 && nvals == 3) {
                        M m = ident();
                        m[12] = key.nums[v], m[13] = key.nums[v + 1], m[14] = key.nums[v + 2];
                        pos[time] = m;
                    }
                    p = v + nvals;
                }
            }
            if (!mat.empty()) {
                for (const auto& kv : mat) t.times.push_back(kv.first), t.keys.push_back(kv.second);
            } else {
                std::set<double> times;
                for (auto* m : {&rot, &scl, &pos})
                    for (const auto& kv : *m) times.insert(kv.first);
                auto at = [](const std::map<double, M>& m, double tm) {
                    if (m.empty()) return ident();
                    auto it2 = m.upper_bound(tm);
                    return it2 == m.begin() ? it2->second : std::prev(it2)->second;
                };
                for (double tm : times) {
                    t.times.push_back(tm);
                    t.keys.push_back(mul(mul(at(scl, tm), at(rot, tm)), at(pos, tm)));
                }
            }
            if (!t.times.empty()) {
                an.ticks = std::max(an.ticks, t.times.back());
                an.tracks.push_back(std::move(t));
            }
        }
        c.anims.push_back(std::move(an));
    }
};

M sample(const CharAnimTrack& t, double tick) {
    if (t.times.size() == 1 || tick <= t.times.front()) return t.keys.front();
    if (tick >= t.times.back()) return t.keys.back();
    const size_t i = size_t(std::upper_bound(t.times.begin(), t.times.end(), tick) - t.times.begin());
    const double a = (tick - t.times[i - 1]) / (t.times[i] - t.times[i - 1]);
    M r;
    // Keys are one frame apart: an element-wise blend is visually indistinguishable from
    // decomposing and slerping, and keeps exact key poses at key times.
    for (size_t k = 0; k < 16; k++) r[k] = t.keys[i - 1][k] * (1 - a) + t.keys[i][k] * a;
    return r;
}

}  // namespace

bool load_character(const Source& a, const std::string& path, Character& c, std::string* err) {
    c = Character{};
    c.path = path;
    XFile x;
    if (!load_x(a, path, x, err)) return false;
    const size_t slash = path.rfind('/');
    Loader l{a, slash == std::string::npos ? "" : path.substr(0, slash + 1), c, {}, {}, {}};
    for (const auto& r : x.roots) {
        if (r.type == "Material" && !r.name.empty()) l.named_materials[r.name] = &r;
        if (r.type == "AnimTicksPerSecond" && !r.nums.empty() && r.nums[0] > 0) c.ticks_per_second = r.nums[0];
        if (r.type == "Frame") l.frames(r, -1);
    }
    for (const auto& r : x.roots) {
        if (r.type == "Frame") l.meshes(r);
        if (r.type == "Mesh") {
            CharNode root;
            root.name = "";
            root.bind = ident();
            c.nodes.push_back(root);
            l.mesh(r, int(c.nodes.size()) - 1);
        }
        if (r.type == "AnimationSet") l.anim_set(r);
    }
    if (c.meshes.empty()) {
        if (err) *err = "no meshes in " + path;
        return false;
    }
    return true;
}

std::vector<std::string> list_characters(const Archive& a) {
    std::vector<std::string> out;
    for (const Entry& e : a.entries()) {
        const std::string low = lower_ascii(e.name);
        // data/<dir>/0N.x, not stages (data/stageNN/...)
        if (low.rfind("data/", 0) != 0 || low.rfind("data/stage", 0) == 0) continue;
        const size_t slash = low.find('/', 5);
        if (slash == std::string::npos || low.find('/', slash + 1) != std::string::npos) continue;
        const std::string file = low.substr(slash + 1);
        if (file.size() == 4 && file[0] == '0' && file[1] >= '0' && file[1] <= '9' && file.compare(2, 2, ".x") == 0)
            out.push_back(e.name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& x, const std::string& y) { return lower_ascii(x) < lower_ascii(y); });
    return out;
}

StageGeometry pose_character(const Character& c, int anim, double seconds) {
    StageGeometry g;
    std::vector<M> local(c.nodes.size());
    for (size_t i = 0; i < c.nodes.size(); i++) local[i] = c.nodes[i].bind;
    if (anim >= 0 && size_t(anim) < c.anims.size()) {
        const CharAnim& an = c.anims[size_t(anim)];
        double tick = seconds * c.ticks_per_second;
        if (an.ticks > 0) tick = std::fmod(tick, an.ticks + 80);  // + one frame: the last key holds
        if (tick < 0) tick = 0;
        for (const auto& t : an.tracks) local[size_t(t.node)] = sample(t, tick);
    }
    std::vector<M> world(c.nodes.size());
    for (size_t i = 0; i < c.nodes.size(); i++)  // parents precede children
        world[i] = c.nodes[i].parent < 0 ? local[i] : mul(local[i], world[size_t(c.nodes[i].parent)]);

    // Device light 0 (0x5D6500): directional along normalize(1, -1, 1); N . -L lights.
    const double inv = 1.0 / std::sqrt(3.0);
    const double to_light[3] = {-inv, inv, -inv};

    for (size_t mi = 0; mi < c.meshes.size(); mi++) {
        const CharMesh& m = c.meshes[mi];
        const size_t nv = m.positions.size() / 3;
        std::vector<double> pos(nv * 3, 0.0), nrm(nv * 3, 0.0);
        auto add = [&](size_t v, const M& x, double w) {
            const double p[3] = {m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]};
            const double n[3] = {m.normals[v * 3], m.normals[v * 3 + 1], m.normals[v * 3 + 2]};
            for (int k = 0; k < 3; k++) {
                pos[v * 3 + size_t(k)] += w * (p[0] * x[size_t(k)] + p[1] * x[size_t(4 + k)] + p[2] * x[size_t(8 + k)] + x[size_t(12 + k)]);
                nrm[v * 3 + size_t(k)] += w * (n[0] * x[size_t(k)] + n[1] * x[size_t(4 + k)] + n[2] * x[size_t(8 + k)]);
            }
        };
        if (m.skin.empty()) {
            const M& x = world[size_t(m.node)];
            for (size_t v = 0; v < nv; v++) add(v, x, 1.0);
        } else {
            std::vector<double> total(nv, 0.0);
            for (const auto& b : m.skin) {
                if (b.node < 0) continue;
                const M x = mul(b.offset, world[size_t(b.node)]);
                for (size_t i = 0; i < b.vertices.size(); i++) {
                    add(b.vertices[i], x, b.weights[i]);
                    total[b.vertices[i]] += b.weights[i];
                }
            }
            for (size_t v = 0; v < nv; v++)  // unweighted vertices stay on the mesh's frame
                if (total[v] <= 0) add(v, world[size_t(m.node)], 1.0);
        }
        for (size_t mat = 0; mat < m.materials.size(); mat++) {
            GBatch b;
            b.slot = 1;
            b.mesh = int(mi);
            b.material = int(mat);
            b.mat = m.materials[mat];
            b.mat.power = 0;  // the stage additive rule does not apply to characters
            b.has_colors = true;
            std::map<uint32_t, uint32_t> remap;
            for (size_t t = 0; t < m.tri_material.size(); t++) {
                if (m.tri_material[t] != mat) continue;
                for (int k = 0; k < 3; k++) {
                    const uint32_t v = m.tri_vertices[t * 3 + size_t(k)];
                    auto it = remap.find(v);
                    if (it == remap.end()) {
                        it = remap.emplace(v, uint32_t(remap.size())).first;
                        double n[3] = {nrm[v * 3], nrm[v * 3 + 1], nrm[v * 3 + 2]};
                        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        const double d = l > 0 ? std::max(0.0, (n[0] * to_light[0] + n[1] * to_light[1] + n[2] * to_light[2]) / l) : 1.0;
                        for (int ch = 0; ch < 3; ch++) {
                            b.positions.push_back(float(pos[v * 3 + size_t(ch)]));
                            const double col = b.mat.emissive[ch] + b.mat.diffuse[ch] * d;
                            b.colors.push_back(uint8_t(std::lround(std::min(1.0, std::max(0.0, col)) * 255)));
                        }
                        b.colors.push_back(uint8_t(std::lround(std::min(1.0f, std::max(0.0f, b.mat.diffuse[3])) * 255)));
                        b.uvs.push_back(m.uvs[v * 2]);
                        b.uvs.push_back(m.uvs[v * 2 + 1]);
                    }
                    b.indices.push_back(it->second);
                }
            }
            if (!b.indices.empty()) g.batches.push_back(std::move(b));
        }
    }
    for (const auto& b : g.batches)
        for (size_t i = 0; i + 2 < b.positions.size(); i += 3) {
            const double p[3] = {b.positions[i], b.positions[i + 1], b.positions[i + 2]};
            g.bounds.add(p);
        }
    return g;
}

}  // namespace db
