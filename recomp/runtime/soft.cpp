// A software Direct3D 9 renderer, for checking frames without a GPU (headless builds).
// It renders only the frames it is asked to capture and writes them as PNG files.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "d3d9.h"
#include "movie.h"
#include "shader.h"
#include "soft.h"

namespace rt {
namespace d3d {

namespace {
using shader::Vec4;

float f32(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
Vec4 color_of(uint32_t argb) {
    return {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
            ((argb >> 24) & 0xFF) / 255.0f};
}
uint32_t pack(const Vec4 &c) {
    auto b = [](float x) { return uint32_t(std::lround(std::min(1.0f, std::max(0.0f, x)) * 255.0f)); };
    return b(c[3]) << 24 | b(c[0]) << 16 | b(c[1]) << 8 | b(c[2]);
}

// ---- PNG (stored deflate blocks) ----
uint32_t crc_table[256];
void crc_init() {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFF) {
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void be32(std::vector<uint8_t> &o, uint32_t v) {
    for (int i = 3; i >= 0; i--) o.push_back(uint8_t(v >> (8 * i)));
}
void chunk(std::vector<uint8_t> &o, const char *type, const std::vector<uint8_t> &data) {
    be32(o, uint32_t(data.size()));
    size_t start = o.size();
    o.insert(o.end(), type, type + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, crc(o.data() + start, o.size() - start) ^ 0xFFFFFFFF);
}
bool write_png(const std::string &path, uint32_t w, uint32_t h, const uint8_t *rgba) {
    static bool init = (crc_init(), true);
    (void)init;
    std::vector<uint8_t> raw;
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + size_t(y) * w * 4, rgba + size_t(y + 1) * w * 4);
    }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(i), raw.begin() + long(i + n));
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return true;
}

// ---- textures ----
struct Decoded {
    uint32_t version = ~0u, w = 0, h = 0;
    std::vector<Vec4> px;
};

void decode_dxt_block(const uint8_t *b, uint32_t fmt, Vec4 out[16]) {
    const uint8_t *cb = fmt == FMT_DXT1 ? b : b + 8;
    uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
    auto rgb = [](uint16_t c) {
        return Vec4{((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f, 1.0f};
    };
    Vec4 p[4] = {rgb(c0), rgb(c1), {}, {}};
    if (fmt != FMT_DXT1 || c0 > c1) {
        for (int i = 0; i < 4; i++) {
            p[2][i] = (2 * p[0][i] + p[1][i]) / 3;
            p[3][i] = (p[0][i] + 2 * p[1][i]) / 3;
        }
    } else {
        for (int i = 0; i < 4; i++) p[2][i] = (p[0][i] + p[1][i]) / 2;
        p[3] = {0, 0, 0, 0};
    }
    uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
    for (int i = 0; i < 16; i++) out[i] = p[(bits >> (2 * i)) & 3];
    if (fmt == FMT_DXT2 || fmt == FMT_DXT3) {
        for (int i = 0; i < 16; i++) out[i][3] = ((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
    } else if (fmt == FMT_DXT4 || fmt == FMT_DXT5) {
        float a[8];
        a[0] = b[0] / 255.0f;
        a[1] = b[1] / 255.0f;
        if (b[0] > b[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
        else {
            for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
            a[6] = 0;
            a[7] = 1;
        }
        uint64_t ab = 0;
        for (int i = 0; i < 6; i++) ab |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++) out[i][3] = a[(ab >> (3 * i)) & 7];
    }
}

void decode(const Image &im, Decoded &d) {
    d.w = im.width;
    d.h = im.height;
    d.version = im.version;
    d.px.assign(size_t(d.w) * d.h, Vec4{1, 0, 1, 1});
    const uint8_t *base = GM + im.mem;
    if (is_dxt(im.format)) {
        for (uint32_t by = 0; by < (d.h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (d.w + 3) / 4; bx++) {
                Vec4 blk[16];
                decode_dxt_block(base + by * im.pitch + bx * (im.format == FMT_DXT1 ? 8 : 16), im.format, blk);
                for (int i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + uint32_t(i % 4), y = by * 4 + uint32_t(i / 4);
                    if (x < d.w && y < d.h) d.px[size_t(y) * d.w + x] = blk[i];
                }
            }
        return;
    }
    for (uint32_t y = 0; y < d.h; y++) {
        const uint8_t *row = base + size_t(y) * im.pitch;
        for (uint32_t x = 0; x < d.w; x++) {
            Vec4 &o = d.px[size_t(y) * d.w + x];
            switch (im.format) {
            case FMT_A8R8G8B8: case FMT_X8R8G8B8: {
                uint32_t v;
                memcpy(&v, row + 4 * x, 4);
                o = color_of(im.format == FMT_X8R8G8B8 ? v | 0xFF000000 : v);
                break;
            }
            case FMT_A8B8G8R8: case FMT_X8B8G8R8: {
                const uint8_t *p = row + 4 * x;
                o = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, im.format == FMT_X8B8G8R8 ? 1.0f : p[3] / 255.0f};
                break;
            }
            case FMT_R5G6B5: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 11) & 31) / 31.0f, ((v >> 5) & 63) / 63.0f, (v & 31) / 31.0f, 1};
                break;
            }
            case FMT_A1R5G5B5: case FMT_X1R5G5B5: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 10) & 31) / 31.0f, ((v >> 5) & 31) / 31.0f, (v & 31) / 31.0f,
                     im.format == FMT_X1R5G5B5 || (v & 0x8000) ? 1.0f : 0.0f};
                break;
            }
            case FMT_A4R4G4B4: {
                uint16_t v = uint16_t(row[2 * x] | row[2 * x + 1] << 8);
                o = {((v >> 8) & 15) / 15.0f, ((v >> 4) & 15) / 15.0f, (v & 15) / 15.0f, ((v >> 12) & 15) / 15.0f};
                break;
            }
            case FMT_A8: o = {0, 0, 0, row[x] / 255.0f}; break;
            case FMT_L8: o = {row[x] / 255.0f, row[x] / 255.0f, row[x] / 255.0f, 1}; break;
            case FMT_A8L8: {
                float l = row[2 * x] / 255.0f;
                o = {l, l, l, row[2 * x + 1] / 255.0f};
                break;
            }
            case FMT_R8G8B8: o = {row[3 * x + 2] / 255.0f, row[3 * x + 1] / 255.0f, row[3 * x] / 255.0f, 1}; break;
            default: break;
            }
        }
    }
}

// ---- vertices ----
struct Vtx {
    Vec4 pos;        // clip space (or screen space with rhw for pre-transformed vertices)
    Vec4 color[2];
    Vec4 tex[8];
    float fog = 1;
    float psize = 1;
};
struct Screen {
    float x, y, z, invw;
    Vtx v;
};

struct Element {
    uint32_t stream, offset, type, usage, index;
};

std::vector<Element> elements_of_fvf(uint32_t fvf) {
    std::vector<Element> e;
    uint32_t off = 0;
    uint32_t posk = fvf & 0x400E;
    if (posk == 0x4) {  // XYZRHW
        e.push_back({0, off, 3, 9, 0});
        off += 16;
    } else if (posk) {
        e.push_back({0, off, posk == 0x4002 ? 3u : 2u, 0, 0});
        off += posk == 0x4002 ? 16 : 12;
        int betas = posk >= 6 && posk <= 0xE ? int((posk - 4) / 2) : 0;
        if (betas) {
            bool last_idx = (fvf & 0x1000) || (fvf & 0x8000);
            int w = last_idx ? betas - 1 : betas;
            if (w > 0) e.push_back({0, off, uint32_t(w - 1), 1, 0});
            off += 4 * uint32_t(w);
            if (last_idx) {
                e.push_back({0, off, (fvf & 0x8000) ? 4u : 5u, 2, 0});
                off += 4;
            }
        }
    }
    if (fvf & 0x10) { e.push_back({0, off, 2, 3, 0}); off += 12; }
    if (fvf & 0x20) { e.push_back({0, off, 0, 4, 0}); off += 4; }
    if (fvf & 0x40) { e.push_back({0, off, 4, 10, 0}); off += 4; }
    if (fvf & 0x80) { e.push_back({0, off, 4, 10, 1}); off += 4; }
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        uint32_t n = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        e.push_back({0, off, n - 1, 5, i});
        off += 4 * n;
    }
    return e;
}

Vec4 fetch(const uint8_t *p, uint32_t type) {
    float f[4];
    switch (type) {
    case 0: case 1: case 2: case 3: {
        Vec4 v{0, 0, 0, 1};
        memcpy(f, p, 4 * (type + 1));
        for (uint32_t i = 0; i <= type; i++) v[i] = f[i];
        return v;
    }
    case 4: return {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f, p[3] / 255.0f};  // D3DCOLOR
    case 5: return {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
    case 6: case 7: {
        int16_t s[4] = {0, 0, 0, 1};
        memcpy(s, p, type == 6 ? 4 : 8);
        return {float(s[0]), float(s[1]), float(s[2]), float(s[3])};
    }
    case 8: return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
    default: return {0, 0, 0, 1};
    }
}

Vec4 mul(const Vec4 &v, const Matrix &m) {
    Vec4 r;
    for (int j = 0; j < 4; j++) r[j] = v[0] * m.m[j] + v[1] * m.m[4 + j] + v[2] * m.m[8 + j] + v[3] * m.m[12 + j];
    return r;
}
Matrix mul(const Matrix &a, const Matrix &b) {
    Matrix r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[4 * i + k] * b.m[4 * k + j];
            r.m[4 * i + j] = s;
        }
    return r;
}

class SoftRenderer : public Renderer {
public:
    explicit SoftRenderer(SoftOptions o) : opt_(std::move(o)) {}

    void clear(Device &d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t) override {
        if (!active(d)) return;
        Target t = target(d);
        if (!t.im) return;
        auto fill = [&](int x0, int y0, int x1, int y1) {
            x0 = std::max(0, x0);
            y0 = std::max(0, y0);
            x1 = std::min(int(t.im->width), x1);
            y1 = std::min(int(t.im->height), y1);
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    if (flags & 1) st32(t.im->mem + uint32_t(y) * t.im->pitch + 4 * uint32_t(x), color);
                    if ((flags & 2) && t.depth) (*t.depth)[size_t(y) * t.im->width + size_t(x)] = z;
                }
        };
        uint32_t *vp = d.st.viewport;
        if (!count) fill(int(vp[0]), int(vp[1]), int(vp[0] + vp[2]), int(vp[1] + vp[3]));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t r = rects + 16 * i;
            fill(int(ld32(r)), int(ld32(r + 4)), int(ld32(r + 8)), int(ld32(r + 12)));
        }
    }

    void draw(Device &d, const Draw &dr) override {
        if (!active(d)) return;
        Target t = target(d);
        if (!t.im || bits_per_pixel(t.im->format) != 32) return;
        dev_ = &d;
        prepare_vertices(d, dr);
        if (verts_.empty()) return;
        std::vector<uint32_t> idx = indices(d, dr);
        assemble(d, dr, idx, t);
    }

    void present(Device &d) override {
        if (!active(d)) return;
        Target t = target_of(d, d.backbuffer);
        if (t.im && movie::current(movie_)) {  // the movie's window sits on top of the frame
            const movie::Frame &m = movie_;
            for (int y = 0; y < m.h; y++)
                for (int x = 0; x < m.w; x++) {
                    int dx = m.x + x, dy = m.y + y;
                    if (dx < 0 || dy < 0 || dx >= int(t.im->width) || dy >= int(t.im->height)) continue;
                    const uint8_t *p = &m.rgba[(size_t(y * m.height / m.h) * m.width + size_t(x * m.width / m.w)) * 4];
                    st32(t.im->mem + uint32_t(dy) * t.im->pitch + 4 * uint32_t(dx),
                         0xFF000000u | uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]);
                }
        }
        if (t.im) {
            std::vector<uint8_t> rgba(size_t(t.im->width) * t.im->height * 4);
            for (uint32_t y = 0; y < t.im->height; y++)
                for (uint32_t x = 0; x < t.im->width; x++) {
                    uint32_t v = ld32(t.im->mem + y * t.im->pitch + 4 * x);
                    uint8_t *o = &rgba[(size_t(y) * t.im->width + x) * 4];
                    o[0] = uint8_t(v >> 16);
                    o[1] = uint8_t(v >> 8);
                    o[2] = uint8_t(v);
                    o[3] = 255;
                }
            char name[64];
            snprintf(name, sizeof name, "/frame_%06llu.png", (unsigned long long)d.frames);
            write_png(opt_.dir + name, t.im->width, t.im->height, rgba.data());
            log("recomp: wrote %s%s (%llu draws so far)", opt_.dir.c_str(), name, (unsigned long long)d.draws);
        }
    }

private:
    struct Target {
        Image *im = nullptr;
        std::vector<float> *depth = nullptr;
    };

    bool active(const Device &d) const {
        if (opt_.every && d.frames % opt_.every == 0) return d.frames >= opt_.first;
        for (auto f : opt_.frames)
            if (f == d.frames) return true;
        return false;
    }

    Target target_of(Device &d, uint32_t surface) {
        Target t;
        auto s = com_as<Surface>(surface);
        if (!s) return t;
        t.im = &s->image();
        return t;
    }
    Target target(Device &d) {
        Target t = target_of(d, d.st.render_target);
        if (t.im && d.st.depth) {
            auto &buf = depth_[d.st.depth];
            if (buf.size() != size_t(t.im->width) * t.im->height) buf.assign(size_t(t.im->width) * t.im->height, 1.0f);
            t.depth = &buf;
        }
        return t;
    }

    const Decoded *texture(uint32_t stage) {
        uint32_t obj = dev_->st.textures[stage];
        if (!obj) return nullptr;
        auto tex = com_as<Texture>(obj);
        if (!tex || tex->levels.empty()) return nullptr;
        const Image &im = tex->levels[0];
        Decoded &d = tex_cache_[im.mem];
        if (d.version != im.version || d.w != im.width || d.h != im.height) decode(im, d);
        return &d;
    }

    Vec4 sample(uint32_t stage, float u, float v) {
        const Decoded *t = texture(stage);
        if (!t || !t->w) return {0, 0, 0, 1};
        const auto &s = dev_->st.samp[stage];
        auto addr = [](float c, uint32_t mode, uint32_t n, int &i) {
            float x = c * float(n) - 0.5f;
            int k = int(std::floor(x));
            auto wrap = [&](int j) {
                switch (mode) {
                case 2: {  // mirror
                    int p = int(n) * 2;
                    j = ((j % p) + p) % p;
                    return j >= int(n) ? p - 1 - j : j;
                }
                case 3: case 4: case 5: return std::min(int(n) - 1, std::max(0, j));
                default: return ((j % int(n)) + int(n)) % int(n);
                }
            };
            i = wrap(k);
            int i2 = wrap(k + 1);
            float frac = x - float(k);
            return std::make_pair(i2, frac);
        };
        int x0, y0;
        auto [x1, fx] = addr(u, s[1], t->w, x0);
        auto [y1, fy] = addr(v, s[2], t->h, y0);
        if (s[5] <= 1) {  // point filtering
            int xi = fx < 0.5f ? x0 : x1, yi = fy < 0.5f ? y0 : y1;
            return t->px[size_t(yi) * t->w + size_t(xi)];
        }
        const Vec4 &a = t->px[size_t(y0) * t->w + size_t(x0)], &b = t->px[size_t(y0) * t->w + size_t(x1)];
        const Vec4 &c = t->px[size_t(y1) * t->w + size_t(x0)], &e = t->px[size_t(y1) * t->w + size_t(x1)];
        Vec4 r;
        for (int i = 0; i < 4; i++) {
            float top = a[i] + (b[i] - a[i]) * fx, bot = c[i] + (e[i] - c[i]) * fx;
            r[i] = top + (bot - top) * fy;
        }
        return r;
    }

    static Vec4 sample_cb(void *ctx, int stage, float u, float v) {
        return static_cast<SoftRenderer *>(ctx)->sample(uint32_t(stage), u, v);
    }

    // ---- vertex processing ----
    void prepare_vertices(Device &d, const Draw &dr) {
        verts_.clear();
        std::vector<Element> el;
        if (d.st.decl) {
            if (auto decl = com_as<VertexDecl>(d.st.decl))
                for (auto &e : decl->elements) el.push_back({e.stream, e.offset, e.type, e.usage, e.usage_index});
        } else {
            el = elements_of_fvf(d.st.fvf);
        }
        bool pretransformed = false;
        for (auto &e : el)
            if (e.usage == 9) pretransformed = true;
        uint32_t first, count;
        if (dr.indexed) {
            first = dr.min_index;
            count = dr.num_vertices;
        } else {
            first = dr.start;
            count = vertex_count(dr.prim, dr.count);
        }
        if (count > 1000000) return;
        auto vs_shader = d.st.vs ? com_as<Shader>(d.st.vs) : nullptr;
        const shader::Program *vs = vs_shader ? &program(*vs_shader) : nullptr;
        base_ = dr.indexed ? int64_t(dr.base_vertex) : 0;
        first_ = first;
        verts_.resize(count);
        Matrix wvp = mul(mul(d.st.transforms[256], d.st.transforms[2]), d.st.transforms[3]);
        Matrix wv = mul(d.st.transforms[256], d.st.transforms[2]);
        pretransformed_ = pretransformed && !vs;
        for (uint32_t i = 0; i < count; i++) {
            int64_t vi = base_ + int64_t(first) + i;
            // Gather inputs by (usage, index).
            std::map<uint32_t, Vec4> in;
            for (auto &e : el) {
                uint32_t addr;
                if (dr.up_vertices) {
                    if (e.stream != 0) continue;
                    addr = dr.up_vertices + uint32_t(vi) * dr.up_stride + e.offset;
                } else {
                    const Stream &s = d.st.streams[e.stream & 15];
                    auto vb = com_as<Buffer>(s.vb);
                    if (!vb) continue;
                    uint64_t o = uint64_t(s.offset) + uint64_t(vi) * s.stride + e.offset;
                    if (o + 4 > vb->length) continue;
                    addr = vb->mem + uint32_t(o);
                }
                in[e.usage << 8 | e.index] = fetch(GM + addr, e.type);
            }
            Vtx &o = verts_[i];
            if (vs) {
                shader::VSIn vin;
                for (auto &v : vin.v) v = {0, 0, 0, 1};
                for (auto &r : vs->inputs) {
                    auto it = in.find(r[1] << 8 | r[2]);
                    if (it != in.end() && r[0] < 16) vin.v[r[0]] = it->second;
                }
                shader::VSOut out;
                shader::run_vs(*vs, d.st.vs_const.data(), vin, out);
                o.pos = out.pos;
                o.color[0] = out.color[0];
                o.color[1] = out.color[1];
                for (int k = 0; k < 8; k++) o.tex[k] = out.tex[k];
                o.fog = out.fog;
                o.psize = out.psize;
            } else {
                fixed_function(d, in, wvp, wv, pretransformed, o);
            }
        }
    }

    static uint32_t vertex_count(uint32_t prim, uint32_t n) {
        switch (prim) {
        case 1: return n;
        case 2: return 2 * n;
        case 3: return n + 1;
        case 4: return 3 * n;
        case 5: case 6: return n + 2;
        default: return 0;
        }
    }

    const shader::Program &program(const Shader &s) {
        auto it = programs_.find(&s);
        if (it == programs_.end()) {
            it = programs_.emplace(&s, shader::decode(s.code.data(), s.code.size())).first;
            if (!it->second.error.empty()) log("recomp: shader: %s", it->second.error.c_str());
        }
        return it->second;
    }

    void fixed_function(Device &d, std::map<uint32_t, Vec4> &in, const Matrix &wvp, const Matrix &wv, bool pretransformed,
                        Vtx &o) {
        auto get = [&](uint32_t usage, uint32_t index, Vec4 def) {
            auto it = in.find(usage << 8 | index);
            return it == in.end() ? def : it->second;
        };
        if (pretransformed) {
            o.pos = get(9, 0, {0, 0, 0, 1});
        } else {
            Vec4 p = get(0, 0, {0, 0, 0, 1});
            p[3] = 1;
            o.pos = mul(p, wvp);
        }
        bool has_c0 = in.count(10u << 8), has_c1 = in.count(10u << 8 | 1);
        Vec4 c0 = get(10, 0, {1, 1, 1, 1}), c1 = get(10, 1, {0, 0, 0, 0});
        if (!pretransformed && d.st.rs[137] && in.count(3u << 8)) {
            lighting(d, get(0, 0, {0, 0, 0, 1}), get(3, 0, {0, 0, 1, 0}), wv, has_c0 ? &c0 : nullptr,
                     has_c1 ? &c1 : nullptr, o);
        } else {
            o.color[0] = c0;
            o.color[1] = c1;
        }
        for (int k = 0; k < 8; k++) {
            uint32_t tci = d.st.tss[k][11] & 0xFFFF;
            o.tex[k] = get(5, tci, {0, 0, 0, 1});
        }
        o.fog = 1;
    }

    void lighting(Device &d, Vec4 p, Vec4 n, const Matrix &wv, const Vec4 *vc0, const Vec4 *vc1, Vtx &o) {
        auto mf = [&](int i) { return f32(d.st.material[i]); };
        Vec4 m_diffuse{mf(0), mf(1), mf(2), mf(3)}, m_ambient{mf(4), mf(5), mf(6), mf(7)};
        Vec4 m_emissive{mf(12), mf(13), mf(14), mf(15)};
        bool colorvertex = d.st.rs[141] != 0;
        auto source = [&](uint32_t rs, const Vec4 &mat) {
            if (!colorvertex) return mat;
            if (rs == 1 && vc0) return *vc0;
            if (rs == 2 && vc1) return *vc1;
            return mat;
        };
        Vec4 diffuse = source(d.st.rs[145], m_diffuse), ambient = source(d.st.rs[147], m_ambient);
        Vec4 emissive = source(d.st.rs[148], m_emissive);
        p[3] = 1;
        Vec4 vp = mul(p, wv);
        Vec4 nn{n[0], n[1], n[2], 0};
        Vec4 vn = mul(nn, wv);
        float len = std::sqrt(vn[0] * vn[0] + vn[1] * vn[1] + vn[2] * vn[2]);
        if (len > 0)
            for (int i = 0; i < 3; i++) vn[i] /= len;
        Vec4 ga = color_of(d.st.rs[139]);
        Vec4 acc_d{0, 0, 0, 0}, acc_a{ga[0], ga[1], ga[2], 0};
        const Matrix &view = d.st.transforms[2];
        for (auto &[l, on] : d.st.lights) {
            if (!on) continue;
            auto lf = [&](int i) { return f32(l.raw[i]); };
            uint32_t type = l.raw[0];
            Vec4 L;
            float att = 1;
            if (type == 3) {
                Vec4 dir = mul(Vec4{lf(16), lf(17), lf(18), 0}, view);
                L = {-dir[0], -dir[1], -dir[2], 0};
            } else {
                Vec4 lp = mul(Vec4{lf(13), lf(14), lf(15), 1}, view);
                L = {lp[0] - vp[0], lp[1] - vp[1], lp[2] - vp[2], 0};
                float dist = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                if (dist > lf(19)) continue;
                att = 1.0f / std::max(1e-6f, lf(21) + lf(22) * dist + lf(23) * dist * dist);
            }
            float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (ll > 0)
                for (int i = 0; i < 3; i++) L[i] /= ll;
            float ndl = std::max(0.0f, vn[0] * L[0] + vn[1] * L[1] + vn[2] * L[2]);
            for (int i = 0; i < 3; i++) {
                acc_d[i] += ndl * lf(1 + i) * att;
                acc_a[i] += lf(9 + i) * att;
            }
        }
        for (int i = 0; i < 3; i++) o.color[0][i] = emissive[i] + ambient[i] * acc_a[i] + diffuse[i] * acc_d[i];
        o.color[0][3] = diffuse[3];
        o.color[1] = {0, 0, 0, 0};
    }

    std::vector<uint32_t> indices(Device &d, const Draw &dr) {
        uint32_t n = vertex_count(dr.prim, dr.count);
        std::vector<uint32_t> out(n);
        if (!dr.indexed) {
            for (uint32_t i = 0; i < n; i++) out[i] = i;
            return out;
        }
        uint32_t mem, size, fmt;
        if (dr.up_indices) {
            mem = dr.up_indices;
            fmt = dr.up_index_format;
            size = 0xFFFFFFFF;
        } else {
            auto ib = com_as<Buffer>(d.st.indices);
            if (!ib) return {};
            mem = ib->mem;
            fmt = ib->format;
            size = ib->length;
        }
        uint32_t w = fmt == FMT_INDEX32 ? 4 : 2;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t o = uint64_t(dr.start + i) * w;
            if (o + w > size) {
                out.resize(i);
                break;
            }
            uint32_t v = w == 4 ? ld32(mem + uint32_t(o)) : ld16(mem + uint32_t(o));
            int64_t rel = int64_t(v) - int64_t(first_);
            out[i] = rel < 0 || rel >= int64_t(verts_.size()) ? 0 : uint32_t(rel);
        }
        return out;
    }

    // ---- primitives ----
    void assemble(Device &d, const Draw &dr, const std::vector<uint32_t> &idx, Target &t) {
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a < verts_.size() && b < verts_.size() && c < verts_.size()) triangle(d, verts_[a], verts_[b], verts_[c], t);
        };
        switch (dr.prim) {
        case 4:
            for (size_t i = 0; i + 2 < idx.size(); i += 3) tri(idx[i], idx[i + 1], idx[i + 2]);
            break;
        case 5:
            for (size_t i = 0; i + 2 < idx.size(); i++)
                if (i & 1) tri(idx[i + 1], idx[i], idx[i + 2]);
                else tri(idx[i], idx[i + 1], idx[i + 2]);
            break;
        case 6:
            for (size_t i = 1; i + 1 < idx.size(); i++) tri(idx[0], idx[i], idx[i + 1]);
            break;
        default:
            break;  // points and lines: not drawn by the software renderer
        }
    }

    Screen project(const Device &d, const Vtx &v) {
        Screen s;
        s.v = v;
        if (pretransformed_) {
            s.x = v.pos[0];
            s.y = v.pos[1];
            s.z = v.pos[2];
            s.invw = v.pos[3];
            return s;
        }
        const uint32_t *vp = d.st.viewport;
        float w = v.pos[3];
        s.invw = 1.0f / w;
        float nx = v.pos[0] * s.invw, ny = v.pos[1] * s.invw, nz = v.pos[2] * s.invw;
        s.x = float(vp[0]) + (nx + 1) * 0.5f * float(vp[2]);
        s.y = float(vp[1]) + (1 - ny) * 0.5f * float(vp[3]);
        s.z = f32(vp[4]) + nz * (f32(vp[5]) - f32(vp[4]));
        return s;
    }

    void triangle(Device &d, const Vtx &a, const Vtx &b, const Vtx &c, Target &t) {
        if (pretransformed_) {
            raster(d, project(d, a), project(d, b), project(d, c), t);
            return;
        }
        // Clip against z >= 0 (and w > 0).
        std::vector<Vtx> poly{a, b, c}, out;
        auto dist = [](const Vtx &v) { return std::min(v.pos[2], v.pos[3] - 1e-5f); };
        for (size_t i = 0; i < poly.size(); i++) {
            const Vtx &p = poly[i], &q = poly[(i + 1) % poly.size()];
            float dp = dist(p), dq = dist(q);
            if (dp >= 0) out.push_back(p);
            if ((dp >= 0) != (dq >= 0)) {
                float k = dp / (dp - dq);
                Vtx m;
                auto lerp4 = [&](const Vec4 &x, const Vec4 &y) {
                    Vec4 r;
                    for (int j = 0; j < 4; j++) r[j] = x[j] + (y[j] - x[j]) * k;
                    return r;
                };
                m.pos = lerp4(p.pos, q.pos);
                m.color[0] = lerp4(p.color[0], q.color[0]);
                m.color[1] = lerp4(p.color[1], q.color[1]);
                for (int j = 0; j < 8; j++) m.tex[j] = lerp4(p.tex[j], q.tex[j]);
                m.fog = p.fog + (q.fog - p.fog) * k;
                out.push_back(m);
            }
        }
        for (size_t i = 1; i + 1 < out.size(); i++) raster(d, project(d, out[0]), project(d, out[i]), project(d, out[i + 1]), t);
    }

    void raster(Device &d, const Screen &A, const Screen &B, const Screen &C, Target &t) {
        float area = (B.x - A.x) * (C.y - A.y) - (C.x - A.x) * (B.y - A.y);
        if (area == 0 || !std::isfinite(area)) return;
        uint32_t cull = d.st.rs[22];
        if ((cull == 3 && area < 0) || (cull == 2 && area > 0)) return;
        int w = int(t.im->width), h = int(t.im->height);
        const uint32_t *vp = d.st.viewport;
        int x0 = std::max({0, int(vp[0]), int(std::floor(std::min({A.x, B.x, C.x})))});
        int y0 = std::max({0, int(vp[1]), int(std::floor(std::min({A.y, B.y, C.y})))});
        int x1 = std::min({w - 1, int(vp[0] + vp[2]) - 1, int(std::ceil(std::max({A.x, B.x, C.x})))});
        int y1 = std::min({h - 1, int(vp[1] + vp[3]) - 1, int(std::ceil(std::max({A.y, B.y, C.y})))});
        if (d.st.rs[174]) {  // scissor
            x0 = std::max(x0, int(d.st.scissor[0]));
            y0 = std::max(y0, int(d.st.scissor[1]));
            x1 = std::min(x1, int(d.st.scissor[2]) - 1);
            y1 = std::min(y1, int(d.st.scissor[3]) - 1);
        }
        if (x0 > x1 || y0 > y1) return;
        float inv_area = 1.0f / area;
        auto ps_shader = d.st.ps ? com_as<Shader>(d.st.ps) : nullptr;
        const shader::Program *ps = ps_shader ? &program(*ps_shader) : nullptr;
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float px = float(x), py = float(y);  // D3D9 samples at integer pixel positions
                float w0 = ((B.x - px) * (C.y - py) - (C.x - px) * (B.y - py)) * inv_area;
                float w1 = ((C.x - px) * (A.y - py) - (A.x - px) * (C.y - py)) * inv_area;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * A.z + w1 * B.z + w2 * C.z;
                size_t pi = size_t(y) * size_t(w) + size_t(x);
                if (t.depth && d.st.rs[7]) {
                    if (!depth_test(d.st.rs[23], z, (*t.depth)[pi])) continue;
                }
                float iw = w0 * A.invw + w1 * B.invw + w2 * C.invw;
                float k0 = w0 * A.invw / iw, k1 = w1 * B.invw / iw, k2 = w2 * C.invw / iw;
                auto interp = [&](const Vec4 &a, const Vec4 &b, const Vec4 &c) {
                    Vec4 r;
                    for (int j = 0; j < 4; j++) r[j] = a[j] * k0 + b[j] * k1 + c[j] * k2;
                    return r;
                };
                shader::PSIn pin;
                bool flat = d.st.rs[9] == 1;
                pin.color[0] = flat ? A.v.color[0] : interp(A.v.color[0], B.v.color[0], C.v.color[0]);
                pin.color[1] = flat ? A.v.color[1] : interp(A.v.color[1], B.v.color[1], C.v.color[1]);
                for (int j = 0; j < 8; j++) pin.tex[j] = interp(A.v.tex[j], B.v.tex[j], C.v.tex[j]);
                Vec4 col;
                if (ps) {
                    if (!shader::run_ps(*ps, d.st.ps_const.data(), pin, sample_cb, this, col)) continue;
                } else {
                    col = stages(d, pin);
                }
                if (d.st.rs[28]) {  // fog
                    float f = 1;
                    if (d.st.rs[35]) {  // table fog: linear in depth
                        float start = f32(d.st.rs[36]), end = f32(d.st.rs[37]);
                        float dist = 1.0f / iw;
                        if (d.st.rs[35] == 3 && end != start) f = (end - dist) / (end - start);
                        else if (d.st.rs[35] == 1) f = std::exp(-f32(d.st.rs[38]) * dist);
                        else if (d.st.rs[35] == 2) f = std::exp(-std::pow(f32(d.st.rs[38]) * dist, 2.0f));
                    } else {
                        f = A.v.fog * k0 + B.v.fog * k1 + C.v.fog * k2;
                    }
                    f = std::min(1.0f, std::max(0.0f, f));
                    Vec4 fc = color_of(d.st.rs[34]);
                    for (int j = 0; j < 3; j++) col[j] = fc[j] + (col[j] - fc[j]) * f;
                }
                if (d.st.rs[15]) {  // alpha test
                    float ref = float(d.st.rs[24] & 0xFF);
                    if (!depth_test(d.st.rs[25], std::round(col[3] * 255.0f), ref)) continue;
                }
                uint32_t addr = t.im->mem + uint32_t(y) * t.im->pitch + 4 * uint32_t(x);
                Vec4 dst = color_of(ld32(addr));
                if (d.st.rs[27]) col = blend(d, col, dst);
                uint32_t mask = d.st.rs[168];
                if (mask != 0xF) {
                    if (!(mask & 1)) col[0] = dst[0];
                    if (!(mask & 2)) col[1] = dst[1];
                    if (!(mask & 4)) col[2] = dst[2];
                    if (!(mask & 8)) col[3] = dst[3];
                }
                st32(addr, pack(col));
                if (t.depth && d.st.rs[7] && d.st.rs[14]) (*t.depth)[pi] = z;
            }
        }
    }

    static bool depth_test(uint32_t func, float a, float b) {
        switch (func) {
        case 1: return false;
        case 2: return a < b;
        case 3: return a == b;
        case 4: return a <= b;
        case 5: return a > b;
        case 6: return a != b;
        case 7: return a >= b;
        default: return true;
        }
    }

    Vec4 factor(uint32_t f, const Vec4 &s, const Vec4 &dd, const Device &d) {
        switch (f) {
        case 1: return {0, 0, 0, 0};
        case 2: return {1, 1, 1, 1};
        case 3: return s;
        case 4: return {1 - s[0], 1 - s[1], 1 - s[2], 1 - s[3]};
        case 5: return {s[3], s[3], s[3], s[3]};
        case 6: return {1 - s[3], 1 - s[3], 1 - s[3], 1 - s[3]};
        case 7: return {dd[3], dd[3], dd[3], dd[3]};
        case 8: return {1 - dd[3], 1 - dd[3], 1 - dd[3], 1 - dd[3]};
        case 9: return dd;
        case 10: return {1 - dd[0], 1 - dd[1], 1 - dd[2], 1 - dd[3]};
        case 11: {
            float k = std::min(s[3], 1 - dd[3]);
            return {k, k, k, 1};
        }
        case 14: return color_of(d.st.rs[193]);
        default: return {1, 1, 1, 1};
        }
    }

    Vec4 blend(const Device &d, const Vec4 &s, const Vec4 &dd) {
        uint32_t sf = d.st.rs[19], df = d.st.rs[20];
        if (sf == 12) { sf = 5; df = 6; }
        if (sf == 13) { sf = 6; df = 5; }
        Vec4 a = factor(sf, s, dd, d), b = factor(df, s, dd, d), r;
        for (int i = 0; i < 4; i++) {
            float x = s[i] * a[i], y = dd[i] * b[i];
            switch (d.st.rs[171]) {
            case 2: r[i] = x - y; break;
            case 3: r[i] = y - x; break;
            case 4: r[i] = std::min(s[i], dd[i]); break;
            case 5: r[i] = std::max(s[i], dd[i]); break;
            default: r[i] = x + y;
            }
        }
        return r;
    }

    // Fixed-function texture stages.
    Vec4 stages(Device &d, const shader::PSIn &in) {
        Vec4 current = in.color[0], temp{0, 0, 0, 0};
        Vec4 tf = color_of(d.st.rs[60]);
        for (uint32_t s = 0; s < 8; s++) {
            const auto &t = d.st.tss[s];
            if (t[1] == 1) break;  // COLOROP DISABLE ends the cascade
            Vec4 tex{0, 0, 0, 1};  // an empty stage reads as opaque black
            if (d.st.textures[s]) {
                const Vec4 &tc = in.tex[s];
                tex = sample(s, tc[0], tc[1]);
            }
            auto arg = [&](uint32_t a) {
                Vec4 v;
                switch (a & 0xF) {
                case 0: v = in.color[0]; break;
                case 1: v = current; break;
                case 2: v = tex; break;
                case 3: v = tf; break;
                case 4: v = in.color[1]; break;
                case 5: v = temp; break;
                case 6: v = color_of(t[32]); break;
                default: v = current;
                }
                if (a & 0x10)
                    for (float &x : v) x = 1 - x;
                if (a & 0x20) v = {v[3], v[3], v[3], v[3]};
                return v;
            };
            auto op = [&](uint32_t o, const Vec4 &a1, const Vec4 &a2, const Vec4 &a0, int i) -> float {
                switch (o) {
                case 2: return a1[i];
                case 3: return a2[i];
                case 4: return a1[i] * a2[i];
                case 5: return std::min(1.0f, 2 * a1[i] * a2[i]);
                case 6: return std::min(1.0f, 4 * a1[i] * a2[i]);
                case 7: return std::min(1.0f, a1[i] + a2[i]);
                case 8: return std::min(1.0f, std::max(0.0f, a1[i] + a2[i] - 0.5f));
                case 9: return std::min(1.0f, std::max(0.0f, 2 * (a1[i] + a2[i] - 0.5f)));
                case 10: return std::max(0.0f, a1[i] - a2[i]);
                case 11: return std::min(1.0f, a1[i] + a2[i] - a1[i] * a2[i]);
                case 12: return a1[i] * in.color[0][3] + a2[i] * (1 - in.color[0][3]);
                case 13: return a1[i] * tex[3] + a2[i] * (1 - tex[3]);
                case 14: return a1[i] * tf[3] + a2[i] * (1 - tf[3]);
                case 15: return std::min(1.0f, a1[i] + a2[i] * (1 - tex[3]));
                case 16: return a1[i] * current[3] + a2[i] * (1 - current[3]);
                case 24: {
                    float dot = 4 * ((a1[0] - .5f) * (a2[0] - .5f) + (a1[1] - .5f) * (a2[1] - .5f) + (a1[2] - .5f) * (a2[2] - .5f));
                    return std::min(1.0f, std::max(0.0f, dot));
                }
                case 25: return std::min(1.0f, a0[i] + a1[i] * a2[i]);
                case 26: return a0[i] * a1[i] + (1 - a0[i]) * a2[i];
                default: return a1[i];
                }
            };
            Vec4 c1 = arg(t[2]), c2 = arg(t[3]), c0 = arg(t[26]);
            Vec4 al1 = arg(t[5]), al2 = arg(t[6]), al0 = arg(t[27]);
            Vec4 r;
            for (int i = 0; i < 3; i++) r[i] = op(t[1], c1, c2, c0, i);
            r[3] = t[4] == 1 ? current[3] : op(t[4], al1, al2, al0, 3);
            if (t[28] == 5) temp = r;
            else current = r;
        }
        if (d.st.rs[29])  // specular
            for (int i = 0; i < 3; i++) current[i] = std::min(1.0f, current[i] + in.color[1][i]);
        return current;
    }

    SoftOptions opt_;
    movie::Frame movie_;
    Device *dev_ = nullptr;
    std::vector<Vtx> verts_;
    int64_t base_ = 0;
    uint32_t first_ = 0;
    bool pretransformed_ = false;
    std::map<uint32_t, std::vector<float>> depth_;
    std::map<uint32_t, Decoded> tex_cache_;
    std::map<const Shader *, shader::Program> programs_;
};
}  // namespace

std::unique_ptr<Renderer> make_soft_renderer(SoftOptions o) { return std::make_unique<SoftRenderer>(std::move(o)); }

}  // namespace d3d
}  // namespace rt
