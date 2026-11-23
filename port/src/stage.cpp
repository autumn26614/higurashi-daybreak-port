#include "stage.h"

// DB_NO_ICONV: set by the iOS app build. iconv lives in a separate libiconv on Apple
// platforms, and the app decodes cp932 itself (kCFStringEncodingDOSJapanese).
#if __has_include(<iconv.h>) && !defined(DB_NO_ICONV)
#include <iconv.h>
#define HAVE_ICONV 1
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace db {

std::string utf8(const std::string& cp932) {
#ifndef HAVE_ICONV
    return cp932;  // names are printed as raw Shift-JIS bytes
#else
    static iconv_t cd = iconv_open("UTF-8", "CP932");
    if (cd == iconv_t(-1)) return cp932;
    std::string out(cp932.size() * 3 + 4, '\0');
    char* in = const_cast<char*>(cp932.data());
    size_t inl = cp932.size();
    char* o = &out[0];
    size_t ol = out.size();
    iconv(cd, nullptr, nullptr, nullptr, nullptr);
    if (iconv(cd, &in, &inl, &o, &ol) == size_t(-1)) return cp932;
    out.resize(out.size() - ol);
    return out;
#endif
}

void Box::add(const double* p) {
    for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
}
void Box::add(const Box& b) {
    if (!b.empty()) add(b.lo), add(b.hi);
}

namespace {

using Mat4 = std::array<double, 16>;

// .x numbers are parsed as doubles. Converting a negative, NaN or huge double to an integer
// is undefined behaviour (and differs between x86 and arm64), so counts go through here:
// the result is in [0, limit].
size_t as_count(double v, size_t limit) {
    if (!(v >= 0)) return 0;  // also catches NaN
    if (v >= double(limit)) return limit;
    return size_t(v);
}

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
bool is_identity(const Mat4& m) {
    Mat4 id = identity();
    for (int i = 0; i < 16; i++)
        if (std::fabs(m[i] - id[i]) > 1e-6) return false;
    return true;
}

MaterialInfo material_info(const XObject& m) {
    MaterialInfo mi;
    for (const auto& k : m.kids) {
        if (k.type == "TextureFilename" && !k.strs.empty()) mi.texture = k.strs[0];
        if (k.type == "EffectInstance" && !k.strs.empty()) mi.effect = k.strs[0];
    }
    return mi;
}

struct Walker {
    std::map<std::string, const XObject*> named_materials;
    Stats st;
    bool want_tree = false;

    void mesh(const XObject& m, const Mat4& world, int depth) {
        st.meshes++;
        const auto& n = m.nums;
        // Malformed-file guards: counts are clamped to what the number list can hold.
        size_t nv = n.empty() ? 0 : as_count(n[0], (n.size() - 1) / 3);
        Box lb, wb;
        for (size_t v = 0; v < nv && 1 + v * 3 + 2 < n.size(); v++) {
            double p[3] = {n[1 + v * 3], n[2 + v * 3], n[3 + v * 3]};
            lb.add(p);
            double w[3];
            for (int c = 0; c < 3; c++)
                w[c] = p[0] * world[0 * 4 + c] + p[1] * world[1 * 4 + c] + p[2] * world[2 * 4 + c] + world[12 + c];
            wb.add(w);
        }
        size_t fi = 1 + nv * 3;
        long nf = fi < n.size() ? long(as_count(n[fi], n.size() - fi - 1)) : 0, tris = 0;
        size_t q = fi + 1;
        for (long f = 0; f < nf && q < n.size(); f++) {
            size_t cnt = as_count(n[q], n.size() - q - 1);
            tris += long(cnt >= 2 ? cnt - 2 : 0);
            q += cnt + 1;
        }
        st.verts += long(nv);
        st.faces += nf;
        st.tris += tris;
        st.local.add(lb);
        st.world.add(wb);
        bool normals = false, uv = false, skin = false, colors = false;
        int mats_here = 0;
        for (const auto& k : m.kids) {
            if (k.type == "MeshNormals") normals = true;
            if (k.type == "MeshVertexColors") colors = true;
            if (k.type == "MeshTextureCoords") uv = true;
            if (k.type == "SkinWeights" || k.type == "XSkinMeshHeader") skin = true;
            if (k.type == "DeclData" || k.type == "FVFData") uv = true;  // packed per-vertex data (may hold UVs)
            if (k.type == "MeshMaterialList") {
                for (const auto& mk : k.kids)
                    if (mk.type == "Material") st.mats.push_back(material_info(mk)), mats_here++;
                for (const auto& r : k.refs) {
                    auto it = named_materials.find(r);
                    st.mats.push_back(it != named_materials.end() ? material_info(*it->second) : MaterialInfo{});
                    mats_here++;
                }
            }
        }
        st.materials += mats_here;
        if (!normals) st.mesh_without_normals++;
        if (!uv) st.mesh_without_uv++;
        if (skin) st.skinned++;
        if (colors) st.with_vertex_colors++;
        if (want_tree) {
            char buf[256];
            snprintf(buf, sizeof buf, "%*sMesh %s: %zu verts, %ld faces (%ld tris), %d materials%s", depth * 2, "",
                     utf8(m.name).c_str(), nv, nf, tris, mats_here, skin ? ", skinned" : "");
            st.tree.push_back(buf);
        }
    }

    void frame(const XObject& f, const Mat4& parent, int depth) {
        st.frames++;
        Mat4 local = identity();
        for (const auto& k : f.kids)
            if (k.type == "FrameTransformMatrix" && k.nums.size() >= 16) std::copy(k.nums.begin(), k.nums.begin() + 16, local.begin());
        if (!is_identity(local)) st.nonidentity_frames++;
        Mat4 world = mul(local, parent);
        if (want_tree) {
            char buf[256];
            snprintf(buf, sizeof buf, "%*sFrame %s%s  T=(%.1f, %.1f, %.1f)", depth * 2, "", utf8(f.name).c_str(),
                     is_identity(local) ? "" : " [xform]", local[12], local[13], local[14]);
            st.tree.push_back(buf);
        }
        for (const auto& k : f.kids) {
            if (k.type == "Frame") frame(k, world, depth + 1);
            if (k.type == "Mesh") mesh(k, world, depth + 1);
        }
    }

    void file(const XFile& x) {
        for (const auto& r : x.roots)
            if (r.type == "Material" && !r.name.empty()) named_materials[r.name] = &r;
        for (const auto& r : x.roots) {
            if (r.type == "Frame") frame(r, identity(), 0);
            if (r.type == "Mesh") mesh(r, identity(), 0);
            if (r.type == "AnimationSet") st.anim_sets++;
        }
    }
};

}  // namespace

Stats analyze_x(const XFile& x, bool want_tree) {
    Walker w;
    w.want_tree = want_tree;
    w.file(x);
    return w.st;
}

bool load_x(const Source& a, const std::string& name, XFile& x, std::string* err) {
    const Entry* e = a.find(name);
    if (!e) {
        *err = "missing";
        return false;
    }
    std::vector<uint8_t> data;
    return a.read(*e, data, err) && parse_x(data, x, err);
}

TexStatus resolve_texture(const Source& a, const std::string& dir, const std::string& tex) {
    TexStatus t;
    std::string path = dir + tex;
    if (path.size() < 4) return t;
    t.archive_name = path.substr(0, path.size() - 3) + "cnv";
    t.found = a.find(t.archive_name) != nullptr;
    std::string base = path.substr(0, path.size() - 4);
    for (int m = 1; m < 8 && a.find(base + "_" + std::to_string(m) + ".cnv"); m++) t.mips = m;
    return t;
}

StageSummary summarize_stage(const Source& a, int stage) {
    StageSummary s;
    s.stage = stage;
    char dirbuf[64];
    snprintf(dirbuf, sizeof dirbuf, "data/stage%02d", stage);
    s.dir = dirbuf;
    std::set<std::string> names;
    for (int slot = 0; slot < 8; slot++) {
        for (int variant = 0; variant < 2; variant++) {
            char nb[32];
            snprintf(nb, sizeof nb, variant ? "%02db.x" : "%02d.x", slot);
            XFile x;
            std::string err;
            if (!load_x(a, s.dir + "/" + nb, x, &err)) {
                if (err != "missing") s.errors.push_back(std::string(nb) + ": " + err);
                continue;
            }
            Stats st = analyze_x(x, false);
            s.meshes[variant] += st.meshes;
            s.tris[variant] += st.tris;
            for (const auto& m : st.mats)
                if (!m.texture.empty()) names.insert(m.texture);
        }
    }
    for (const auto& n : names) s.textures.push_back({n, resolve_texture(a, s.dir + "/", n)});
    return s;
}

}  // namespace db
