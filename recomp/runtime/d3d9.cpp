// Direct3D 9 COM objects for the recompiled game. Resource memory is guest memory (the game
// locks and writes it directly); drawing goes to a Renderer.
#include "d3d9.h"

#include <fstream>

#include "msl.h"
#include "shader.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace rt {
namespace d3d {

namespace {
std::unique_ptr<Renderer> g_renderer;

class NullRenderer : public Renderer {
public:
    void clear(Device &, uint32_t, uint32_t, uint32_t, uint32_t, float, uint32_t) override {}
    void draw(Device &, const Draw &) override {}
    void present(Device &d) override {
        if (d.frames % 60 == 0) log("recomp: frame %llu (%llu draws)", (unsigned long long)d.frames, (unsigned long long)d.draws);
    }
};

Renderer &renderer() {
    if (!g_renderer) g_renderer = std::make_unique<NullRenderer>();
    return *g_renderer;
}

float f32(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
uint32_t fbits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

// A texture level's surface counts on its texture (see surface_add_ref).
uint32_t counted(uint32_t o) {
    if (auto s = com_as<Surface>(o))
        if (s->parent_self) return s->parent_self;
    return o;
}
void add_ref(uint32_t o) {
    if (auto p = com_get(counted(o))) p->refs++;
}
void release(uint32_t o) {
    o = counted(o);
    if (auto p = com_get(o))
        if (--p->refs == 0) com_release_all(o);
}
void bind(uint32_t &slot, uint32_t o) {
    if (o == slot) return;
    if (o) add_ref(o);
    uint32_t old = slot;
    slot = o;
    if (old) release(old);
}

constexpr uint32_t D3D_OK = 0, D3DERR_INVALIDCALL = 0x8876086C, D3DERR_NOTAVAILABLE = 0x8876086A;
}  // namespace

void set_renderer(std::unique_ptr<Renderer> r) { g_renderer = std::move(r); }

uint32_t next_version() {
    static std::atomic<uint32_t> v{1};
    return v++;
}

bool is_dxt(uint32_t f) { return f == FMT_DXT1 || f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5; }

uint32_t bits_per_pixel(uint32_t f) {
    switch (f) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_A8B8G8R8: case FMT_X8B8G8R8: case FMT_D32: case FMT_D24S8:
    case FMT_D24X8: case FMT_INDEX32: return 32;
    case FMT_R8G8B8: return 24;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_A8L8: case FMT_D16:
    case FMT_INDEX16: return 16;
    case FMT_A8: case FMT_L8: case FMT_P8: return 8;
    case FMT_DXT1: return 4;
    default: return is_dxt(f) ? 8 : 32;
    }
}
uint32_t pitch_of(uint32_t f, uint32_t w) {
    if (is_dxt(f)) return ((w + 3) / 4) * (f == FMT_DXT1 ? 8 : 16);
    return (w * bits_per_pixel(f) / 8 + 3) & ~3u;
}
uint32_t rows_of(uint32_t f, uint32_t h) { return is_dxt(f) ? (h + 3) / 4 : h; }

static Image make_image(uint32_t w, uint32_t h, uint32_t f) {
    Image im;
    im.width = w ? w : 1;
    im.height = h ? h : 1;
    im.format = f;
    im.pitch = pitch_of(f, im.width);
    im.size = im.pitch * rows_of(f, im.height);
    im.mem = galloc(im.size, true);
    return im;
}

Image &Surface::image() { return parent ? parent->levels[level] : own; }
Surface::~Surface() {
    if (!parent && own.mem) gfree(own.mem);
}
Texture::~Texture() {
    for (auto &l : levels) gfree(l.mem);
    for (uint32_t s : surfaces)
        if (s) com_release_all(s);
}
Buffer::~Buffer() { gfree(mem); }

// ---------------------------------------------------------------------------------------
// Vtables. Methods without an implementation stop the program with their name.
#define M(name, n) {#name, n, nullptr}
#define I(name, n, ...) {#name, n, __VA_ARGS__}

static uint32_t resource_get_device(CPU *c) {
    auto r = com_this<DeviceChild>(c);
    add_ref(r->device);
    st32(arg(c, 1), r->device);
    return D3D_OK;
}
static uint32_t resource_get_type(CPU *c) { return com_this<Resource>(c)->type; }
static uint32_t ok(CPU *) { return D3D_OK; }
static uint32_t zero(CPU *) { return 0; }

#define RESOURCE_METHODS                                                                       \
    COM_IUNKNOWN, I(GetDevice, 2, resource_get_device), M(SetPrivateData, 5), M(GetPrivateData, 4), \
        M(FreePrivateData, 2), I(SetPriority, 2, zero), I(GetPriority, 1, zero), I(PreLoad, 1, zero), \
        I(GetType, 1, resource_get_type)

// ---- surfaces ----
static uint32_t lock_rect(Image &im, uint32_t locked, uint32_t rect) {
    uint32_t x = 0, y = 0;
    if (rect) {
        x = ld32(rect);
        y = ld32(rect + 4);
    }
    uint32_t off = rows_of(im.format, y) * im.pitch + (is_dxt(im.format) ? (x / 4) * (im.format == FMT_DXT1 ? 8 : 16)
                                                                          : x * bits_per_pixel(im.format) / 8);
    st32(locked, im.pitch);
    st32(locked + 4, im.mem + off);
    return D3D_OK;
}
static void write_desc(uint32_t d, const Image &im, uint32_t type, uint32_t usage, uint32_t pool) {
    st32(d, im.format);
    st32(d + 4, type);
    st32(d + 8, usage);
    st32(d + 12, pool);
    st32(d + 16, 0);  // multisample
    st32(d + 20, 0);
    st32(d + 24, im.width);
    st32(d + 28, im.height);
}
// A texture level's surface has no reference count of its own: AddRef and Release go to the
// texture (Direct3D's containers), so the texture lives while its surfaces are held.
static uint32_t surface_add_ref(CPU *c);
static uint32_t surface_release(CPU *c);

static const Method surface_vt[] = {
    {"QueryInterface", 3, ::rt::com_query_interface}, {"AddRef", 1, surface_add_ref},
    {"Release", 1, surface_release}, I(GetDevice, 2, resource_get_device), M(SetPrivateData, 5),
    M(GetPrivateData, 4), M(FreePrivateData, 2), I(SetPriority, 2, zero), I(GetPriority, 1, zero),
    I(PreLoad, 1, zero), I(GetType, 1, resource_get_type),
    I(GetContainer, 3, [](CPU *c) {
        auto s = com_this<Surface>(c);
        uint32_t o = s->parent_self ? s->parent_self : s->device;
        add_ref(o);
        st32(arg(c, 2), o);
        return D3D_OK;
    }),
    I(GetDesc, 2, [](CPU *c) {
        auto s = com_this<Surface>(c);
        write_desc(arg(c, 1), s->image(), 1, s->usage, s->pool);
        return D3D_OK;
    }),
    I(LockRect, 4, [](CPU *c) { return lock_rect(com_this<Surface>(c)->image(), arg(c, 1), arg(c, 2)); }),
    I(UnlockRect, 1, [](CPU *c) {
        com_this<Surface>(c)->image().version = next_version();
        return D3D_OK;
    }),
    M(GetDC, 2), M(ReleaseDC, 2),
};

static uint32_t surface_add_ref(CPU *c) {
    auto s = com_this<Surface>(c);
    if (s->parent_self) {
        add_ref(s->parent_self);
        auto t = com_get(s->parent_self);
        return t ? t->refs : 0;
    }
    return ++s->refs;
}
static uint32_t surface_release(CPU *c) {
    auto s = com_this<Surface>(c);
    if (s->parent_self) {
        auto t = com_get(s->parent_self);
        uint32_t r = t ? t->refs - 1 : 0;
        release(s->parent_self);
        return r;
    }
    uint32_t r = --s->refs;
    if (r == 0) com_release_all(s->self);
    return r;
}

static uint32_t new_surface(uint32_t device, Image im, uint32_t usage, uint32_t pool) {
    auto s = std::make_shared<Surface>();
    s->type = 1;
    s->device = device;
    s->own = im;
    s->usage = usage;
    s->pool = pool;
    s->iids = {"{0CFBAF3A-9FF6-429A-99B3-A2796AF8B89B}"};
    return com_new(s, com_vtable("IDirect3DSurface9", surface_vt));
}

// ---- textures ----
static const Method texture_vt[] = {
    RESOURCE_METHODS,
    I(SetLOD, 2, zero), I(GetLOD, 1, zero),
    I(GetLevelCount, 1, [](CPU *c) { return uint32_t(com_this<Texture>(c)->levels.size()); }),
    I(SetAutoGenFilterType, 2, ok), I(GetAutoGenFilterType, 1, [](CPU *) { return 2u; }),
    I(GenerateMipSubLevels, 1, zero),
    I(GetLevelDesc, 3, [](CPU *c) {
        auto t = com_this<Texture>(c);
        uint32_t l = arg(c, 1);
        if (l >= t->levels.size()) return D3DERR_INVALIDCALL;
        write_desc(arg(c, 2), t->levels[l], 3, t->usage, t->pool);
        return D3D_OK;
    }),
    I(GetSurfaceLevel, 3, [](CPU *c) {
        auto t = com_this<Texture>(c);
        uint32_t l = arg(c, 1);
        if (l >= t->levels.size()) return D3DERR_INVALIDCALL;
        if (!t->surfaces[l]) {
            auto s = std::make_shared<Surface>();
            s->type = 1;
            s->device = t->device;
            s->parent = t.get();
            s->parent_self = t->self;
            s->level = l;
            s->usage = t->usage;
            s->pool = t->pool;
            t->surfaces[l] = com_new(s, com_vtable("IDirect3DSurface9", surface_vt));  // the texture's reference
        }
        add_ref(t->self);  // the surface's reference is the texture's
        st32(arg(c, 2), t->surfaces[l]);
        return D3D_OK;
    }),
    I(LockRect, 5, [](CPU *c) {
        auto t = com_this<Texture>(c);
        uint32_t l = arg(c, 1);
        if (l >= t->levels.size()) return D3DERR_INVALIDCALL;
        return lock_rect(t->levels[l], arg(c, 2), arg(c, 3));
    }),
    I(UnlockRect, 2, [](CPU *c) {
        auto t = com_this<Texture>(c);
        if (arg(c, 1) < t->levels.size()) t->levels[arg(c, 1)].version = next_version();
        return D3D_OK;
    }),
    I(AddDirtyRect, 2, ok),
};

static uint32_t create_texture(uint32_t device, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                               uint32_t format, uint32_t pool) {
    auto t = std::make_shared<Texture>();
    t->type = 3;
    t->device = device;
    t->usage = usage;
    t->pool = pool;
    uint32_t maxl = 1;
    for (uint32_t s = std::max(w, h); s > 1; s >>= 1) maxl++;
    if (!levels || levels > maxl) levels = maxl;
    if (usage & 0x400) levels = 1;  // D3DUSAGE_AUTOGENMIPMAP: one visible level
    for (uint32_t i = 0; i < levels; i++)
        t->levels.push_back(make_image(std::max(1u, w >> i), std::max(1u, h >> i), format));
    t->surfaces.assign(levels, 0);
    t->iids = {"{85C31227-3DE5-4F00-9B3A-F11AC38C18B5}", "{580CA87E-1D3C-4D54-991D-B7D3E3C298CE}"};
    return com_new(t, com_vtable("IDirect3DTexture9", texture_vt));
}

// ---- buffers ----
static const Method buffer_vt[] = {
    RESOURCE_METHODS,
    I(Lock, 5, [](CPU *c) {
        auto b = com_this<Buffer>(c);
        st32(arg(c, 3), b->mem + arg(c, 1));
        return D3D_OK;
    }),
    I(Unlock, 1, [](CPU *c) {
        com_this<Buffer>(c)->version = next_version();
        return D3D_OK;
    }),
    I(GetDesc, 2, [](CPU *c) {
        auto b = com_this<Buffer>(c);
        uint32_t d = arg(c, 1);
        st32(d, b->type == 6 ? 100u : b->format);  // D3DFMT_VERTEXDATA
        st32(d + 4, b->type);
        st32(d + 8, b->usage);
        st32(d + 12, b->pool);
        st32(d + 16, b->length);
        if (b->type == 6) st32(d + 20, b->fvf);
        return D3D_OK;
    }),
};

static uint32_t create_buffer(uint32_t device, uint32_t type, uint32_t length, uint32_t usage, uint32_t fvf_or_format,
                              uint32_t pool) {
    auto b = std::make_shared<Buffer>();
    b->type = type;
    b->device = device;
    b->length = length;
    b->usage = usage;
    b->pool = pool;
    if (type == 6) b->fvf = fvf_or_format;
    else b->format = fvf_or_format;
    b->mem = galloc(length ? length : 4, true);
    b->iids = {type == 6 ? "{B64BB1B5-FD70-4DF6-BF91-19D0A12455E3}" : "{7C9DD65E-D3F7-4529-ACEE-785830ACDE35}"};
    return com_new(b, com_vtable(type == 6 ? "IDirect3DVertexBuffer9" : "IDirect3DIndexBuffer9", buffer_vt));
}

// ---- declarations and shaders ----
static const Method decl_vt[] = {
    COM_IUNKNOWN, I(GetDevice, 2, resource_get_device),
    I(GetDeclaration, 3, [](CPU *c) {
        auto d = com_this<VertexDecl>(c);
        uint32_t n = uint32_t(d->elements.size()) + 1;
        if (arg(c, 1)) {
            for (uint32_t i = 0; i < n - 1; i++) {
                const auto &e = d->elements[i];
                uint32_t o = arg(c, 1) + 8 * i;
                st16(o, e.stream);
                st16(o + 2, e.offset);
                st8(o + 4, e.type);
                st8(o + 5, e.method);
                st8(o + 6, e.usage);
                st8(o + 7, e.usage_index);
            }
            uint32_t o = arg(c, 1) + 8 * (n - 1);
            st16(o, 0xFF);
            st16(o + 2, 0);
            st8(o + 4, 17);  // D3DDECLTYPE_UNUSED
            st8(o + 5, 0);
            st8(o + 6, 0);
            st8(o + 7, 0);
        }
        st32(arg(c, 2), n);
        return D3D_OK;
    }),
};
static const Method shader_vt[] = {
    COM_IUNKNOWN, I(GetDevice, 2, resource_get_device),
    I(GetFunction, 3, [](CPU *c) {
        auto s = com_this<Shader>(c);
        uint32_t bytes = uint32_t(s->code.size() * 4);
        if (arg(c, 1)) memcpy(GM + arg(c, 1), s->code.data(), bytes);
        st32(arg(c, 2), bytes);
        return D3D_OK;
    }),
};

// RECOMP_DUMP_MSL=<dir>: write every shader the game creates as Metal source (checking the
// translation without a Mac).
static void dump_msl(const Shader &s) {
    const char *dir = getenv("RECOMP_DUMP_MSL");
    if (!dir) return;
    static int n = 0;
    static bool common = false;
    if (!common) {
        common = true;
        std::ofstream(std::string(dir) + "/common.metal") << msl::prelude() << msl::fixed_function();
    }
    shader::Program p = shader::decode(s.code.data(), s.code.size());
    std::string err, name = std::string(s.pixel ? "ps_" : "vs_") + std::to_string(n++);
    std::string src = s.pixel ? msl::fragment(p, name, &err) : msl::vertex(p, name, &err);
    if (!err.empty() || !p.error.empty()) log("recomp: %s does not translate: %s%s", name.c_str(), p.error.c_str(), err.c_str());
    std::ofstream f(std::string(dir) + "/" + name + ".metal");
    f << src << "/* decoded:\n";
    for (const auto &in : p.ins) {
        char b[160];
        snprintf(b, sizeof b, "op %u%s dst t%u n%u mask %x sat %d shift %d | src", in.op, in.coissue ? " +" : "",
                 in.d.type, in.d.num, in.d.mask, in.d.sat, in.d.shift);
        f << b;
        for (int k = 0; k < in.nsrc; k++)
            f << " t" << int(in.s[k].type) << " n" << in.s[k].num << " mod" << int(in.s[k].mod) << " swz"
              << int(in.s[k].swz[0]) << int(in.s[k].swz[1]) << int(in.s[k].swz[2]) << int(in.s[k].swz[3]);
        f << "\n";
    }
    f << "*/\n";
}

static uint32_t shader_length(uint32_t a) {  // tokens up to and including the end token
    uint32_t i = 1;
    for (; i < 65536; i++) {
        uint32_t t = ld32(a + 4 * i);
        if (t == 0x0000FFFF) return i + 1;
        if ((t & 0xFFFF) == 0xFFFE) i += (t >> 16) & 0x7FFF;  // comment block
    }
    return i;
}

// ---- state blocks ----
// A block is a State in which the fields that are not part of it hold kUnset. Recording
// (BeginStateBlock .. EndStateBlock) fills the device state with kUnset, lets the setters
// write as usual, keeps what they wrote as the block and puts the device state back:
// recorded calls do not take effect, and a state set to its current value still counts
// (D3DX effects record their states that way).
constexpr uint32_t kUnset = 0x7FBADBADu;

static void fill_unset(State &s) {
    s.rs.fill(kUnset);
    for (auto &t : s.tss) t.fill(kUnset);
    for (auto &t : s.samp) t.fill(kUnset);
    s.textures.fill(kUnset);
    for (auto &m : s.transforms) memcpy(m.m, &kUnset, 4);
    s.viewport[0] = kUnset;
    s.material[0] = kUnset;
    s.lights.clear();
    for (auto &st : s.streams) st.vb = kUnset;
    s.indices = s.fvf = s.decl = s.vs = s.ps = kUnset;
    for (size_t i = 0; i < s.vs_const.size(); i += 4) memcpy(&s.vs_const[i], &kUnset, 4);
    for (size_t i = 0; i < s.ps_const.size(); i += 4) memcpy(&s.ps_const[i], &kUnset, 4);
    for (size_t i = 0; i < s.vs_const_i.size(); i += 4) s.vs_const_i[i] = int32_t(kUnset);
    s.vs_const_b.fill(kUnset);
    s.scissor[0] = kUnset;
}

static bool set_bits(const void *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v != kUnset;
}

// Copies the fields of `block` that are part of it, from `from` into `to`. Object slots on
// the device go through bind() (references).
static void copy_marked(const State &block, const State &from, State &to, bool to_device) {
    auto obj = [&](uint32_t &slot, uint32_t v) {
        if (to_device) bind(slot, v);
        else slot = v;
    };
    for (int i = 0; i < 256; i++)
        if (block.rs[i] != kUnset) to.rs[i] = from.rs[i];
    for (int s = 0; s < 8; s++)
        for (int i = 0; i < 33; i++)
            if (block.tss[s][i] != kUnset) to.tss[s][i] = from.tss[s][i];
    for (int s = 0; s < 16; s++)
        for (int i = 0; i < 14; i++)
            if (block.samp[s][i] != kUnset) to.samp[s][i] = from.samp[s][i];
    for (int i = 0; i < 16; i++) {
        if (block.textures[i] != kUnset) obj(to.textures[i], from.textures[i]);
        if (block.streams[i].vb != kUnset) {
            obj(to.streams[i].vb, from.streams[i].vb);
            to.streams[i].offset = from.streams[i].offset;
            to.streams[i].stride = from.streams[i].stride;
        }
    }
    for (size_t i = 0; i < block.transforms.size(); i++)
        if (set_bits(block.transforms[i].m)) to.transforms[i] = from.transforms[i];
    if (block.viewport[0] != kUnset) memcpy(to.viewport, from.viewport, sizeof to.viewport);
    if (block.material[0] != kUnset) memcpy(to.material, from.material, sizeof to.material);
    if (block.scissor[0] != kUnset) memcpy(to.scissor, from.scissor, sizeof to.scissor);
    if (!block.lights.empty()) to.lights = from.lights;
    if (block.indices != kUnset) obj(to.indices, from.indices);
    if (block.decl != kUnset) obj(to.decl, from.decl);
    if (block.vs != kUnset) obj(to.vs, from.vs);
    if (block.ps != kUnset) obj(to.ps, from.ps);
    if (block.fvf != kUnset) to.fvf = from.fvf;
    for (size_t i = 0; i < block.vs_const.size(); i += 4)
        if (set_bits(&block.vs_const[i])) memcpy(&to.vs_const[i], &from.vs_const[i], 16);
    for (size_t i = 0; i < block.ps_const.size(); i += 4)
        if (set_bits(&block.ps_const[i])) memcpy(&to.ps_const[i], &from.ps_const[i], 16);
    for (size_t i = 0; i < block.vs_const_i.size(); i += 4)
        if (uint32_t(block.vs_const_i[i]) != kUnset) memcpy(&to.vs_const_i[i], &from.vs_const_i[i], 16);
    for (size_t i = 0; i < block.vs_const_b.size(); i++)
        if (block.vs_const_b[i] != kUnset) to.vs_const_b[i] = from.vs_const_b[i];
}

// The objects a block holds (textures, buffers, declaration, shaders), as Direct3D's
// blocks hold references to them.
template <class F> static void block_objects(const State &s, F f) {
    for (int i = 0; i < 16; i++) {
        if (s.textures[i] != kUnset && s.textures[i]) f(s.textures[i]);
        if (s.streams[i].vb != kUnset && s.streams[i].vb) f(s.streams[i].vb);
    }
    for (uint32_t o : {s.indices, s.decl, s.vs, s.ps})
        if (o != kUnset && o) f(o);
}

struct StateBlock : DeviceChild {
    State st;  // the values, with kUnset where the block does not cover a field
    ~StateBlock() override { block_objects(st, release); }
};
static std::shared_ptr<Device> device_of(uint32_t self) { return com_as<Device>(self); }
static const Method stateblock_vt[] = {
    COM_IUNKNOWN, I(GetDevice, 2, resource_get_device),
    I(Capture, 1, [](CPU *c) {
        auto b = com_this<StateBlock>(c);
        if (auto d = device_of(b->device)) {
            block_objects(b->st, release);
            copy_marked(b->st, d->st, b->st, false);
            block_objects(b->st, add_ref);
        }
        return D3D_OK;
    }),
    I(Apply, 1, [](CPU *c) {
        auto b = com_this<StateBlock>(c);
        if (auto d = device_of(b->device)) copy_marked(b->st, b->st, d->st, true);
        return D3D_OK;
    }),
};

static uint32_t new_state_block(uint32_t device, const State &st) {
    auto b = std::make_shared<StateBlock>();
    b->device = device;
    b->st = st;
    block_objects(b->st, add_ref);
    return com_new(b, com_vtable("IDirect3DStateBlock9", stateblock_vt));
}

// ---- device ----
static std::shared_ptr<Device> dev(CPU *c) { return com_this<Device>(c); }

static void default_state(State &s) {
    auto &rs = s.rs;
    rs[7] = 1;  // ZENABLE (auto depth buffer)
    rs[8] = 3;
    rs[9] = 2;
    rs[14] = 1;
    rs[16] = 1;
    rs[19] = 2;
    rs[20] = 1;
    rs[22] = 3;
    rs[23] = 4;
    rs[25] = 8;
    rs[37] = fbits(1.0f);
    rs[38] = fbits(1.0f);
    rs[53] = rs[54] = rs[55] = 1;
    rs[56] = 8;
    rs[58] = rs[59] = 0xFFFFFFFF;
    rs[60] = 0xFFFFFFFF;
    rs[136] = 1;
    rs[137] = 1;
    rs[141] = 1;
    rs[142] = 1;
    rs[145] = 1;
    rs[146] = 2;
    rs[154] = rs[155] = fbits(1.0f);
    rs[158] = fbits(1.0f);
    rs[161] = 1;
    rs[162] = 0xFFFFFFFF;
    rs[166] = fbits(64.0f);
    rs[168] = 0xF;
    rs[171] = 1;
    rs[172] = 3;
    rs[173] = 1;
    rs[186] = rs[187] = rs[188] = 1;
    rs[189] = 8;
    rs[190] = rs[191] = rs[192] = 0xF;
    rs[193] = 0xFFFFFFFF;
    rs[207] = 2;
    rs[208] = 1;
    rs[209] = 1;
    for (uint32_t i = 0; i < 8; i++) {
        auto &t = s.tss[i];
        t[1] = i == 0 ? 4 : 1;
        t[2] = 2;
        t[3] = 1;
        t[4] = i == 0 ? 2 : 1;
        t[5] = 2;
        t[6] = 1;
        t[11] = i;
        t[26] = t[27] = t[28] = 1;
    }
    for (auto &sm : s.samp) {
        sm[1] = sm[2] = sm[3] = 1;
        sm[5] = sm[6] = 1;
        sm[10] = 1;
    }
    for (auto &m : s.transforms) {
        memset(m.m, 0, sizeof m.m);
        m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1;
    }
    s.material[0] = s.material[1] = s.material[2] = s.material[3] = fbits(1.0f);  // diffuse white
}

// RECOMP_DRAWLOG=<frame>: describe every draw of that frame.
static void log_draw(Device &d, const Draw &dr) {
    static long long want = getenv("RECOMP_DRAWLOG") ? atoll(getenv("RECOMP_DRAWLOG")) : -1;
    if (want < 0 || (long long)d.frames != want) return;
    const State &s = d.st;
    std::string v;
    if (dr.up_vertices) {
        char b[160];
        snprintf(b, sizeof b, " v0=[%.1f %.1f %.3f %.3f c=%08x] stride %u", f32(ld32(dr.up_vertices)),
                 f32(ld32(dr.up_vertices + 4)), f32(ld32(dr.up_vertices + 8)), f32(ld32(dr.up_vertices + 12)),
                 ld32(dr.up_vertices + 16), dr.up_stride);
        v = b;
    }
    uint32_t tex = s.textures[0];
    auto t = com_as<Texture>(tex);
    log("draw prim %u x%u%s fvf %x decl %x vs %x ps %x tex0 %ux%u fmt %u | blend %u %u/%u op %u atest %u ref %u "
        "func %u z %u/%u | cop %u(%x,%x) aop %u(%x,%x) cop1 %u tf %08x light %u%s",
        dr.prim, dr.count, dr.indexed ? " indexed" : "", s.fvf, s.decl, s.vs, s.ps, t ? t->levels[0].width : 0,
        t ? t->levels[0].height : 0, t ? t->levels[0].format : 0, s.rs[27], s.rs[19], s.rs[20], s.rs[171], s.rs[15],
        s.rs[24], s.rs[25], s.rs[7], s.rs[14], s.tss[0][1], s.tss[0][2], s.tss[0][3], s.tss[0][4], s.tss[0][5],
        s.tss[0][6], s.tss[1][1], s.rs[60], s.rs[137], v.c_str());
}

static uint32_t draw(CPU *c, const Draw &dr) {
    auto d = dev(c);
    d->draws++;
    log_draw(*d, dr);
    renderer().draw(*d, dr);
    return D3D_OK;
}

static Matrix load_matrix(uint32_t a) {
    Matrix m;
    memcpy(m.m, GM + a, 64);
    return m;
}

static void fill_caps(uint32_t p) {
    memset(GM + p, 0, 304);
    auto put = [&](uint32_t off, uint32_t v) { st32(p + off, v); };
    auto putf = [&](uint32_t off, float v) { st32(p + off, fbits(v)); };
    put(0, 1);              // HAL
    put(8, 0x20000);        // Caps: READ_SCANLINE
    put(12, 0x60020000);    // Caps2: FULLSCREENGAMMA, DYNAMICTEXTURES, CANAUTOGENMIPMAP
    put(16, 0x3A0);         // Caps3
    put(20, 0x8000000F);    // presentation intervals
    put(24, 1);
    put(28, 0x1BBEF0);      // DevCaps: hardware T&L, pure device, ...
    put(32, 0x2CEF2);       // PrimitiveMiscCaps
    put(36, 0x07732191);    // RasterCaps
    put(40, 0xFF);
    put(44, 0x3FFF);
    put(48, 0x3FFF);
    put(52, 0xFF);
    put(56, 0x84208);       // ShadeCaps
    put(60, 0x1EC05);       // TextureCaps (no POW2 restriction)
    put(64, 0x07030700);
    put(68, 0x03030300);
    put(72, 0x03030300);
    put(76, 0x3F);
    put(80, 0x3F);
    put(84, 0x1F);
    put(88, 4096);
    put(92, 4096);
    put(96, 512);
    put(100, 8192);
    put(104, 4096);
    put(108, 16);
    putf(112, 1e10f);
    putf(116, -1e8f);
    putf(120, -1e8f);
    putf(124, 1e8f);
    putf(128, 1e8f);
    put(136, 0x1FF);
    put(140, 0x80008);
    put(144, 0x03FFFFFF);
    put(148, 8);
    put(152, 8);
    put(156, 0x7B);
    put(160, 8);
    put(164, 6);
    put(168, 4);
    put(172, 0);
    putf(176, 256.0f);
    put(180, 0xFFFFF);
    put(184, 0xFFFFFF);
    put(188, 16);
    put(192, 255);
    put(196, 0xFFFE0200);   // vs_2_0
    put(200, 256);
    put(204, 0xFFFF0200);   // ps_2_0
    putf(208, 8.0f);
    put(212, 0x41);
    put(232, 1);
    put(236, 0x30F);
    put(240, 1);
    put(244, 0x03000300);
    put(248 + 8, 12);       // VS20Caps.NumTemps
    put(264 + 8, 12);       // PS20Caps.NumTemps
    put(264 + 16, 96);      // PS20Caps.NumInstructionSlots
}

// Present waits for the next 60 Hz tick, as vsync does (PresentationInterval ONE).
static void wait_vsync() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    next += std::chrono::microseconds(16667);
    if (next < now - std::chrono::milliseconds(100)) next = now;  // fell far behind: resync
    std::this_thread::sleep_until(next);
}

static uint32_t present(CPU *c) {
    auto d = dev(c);
    renderer().present(*d);
    d->frames++;
    if (getenv("RECOMP_FPS") && d->frames % 600 == 0) {  // RECOMP_FPS: frame rate every 600 frames
        static auto last = std::chrono::steady_clock::now();
        static uint64_t last_draws = 0;
        static bool first = true;
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        if (!first) log("recomp: frame %llu: %.1f fps, %.0f draws/frame, guest heap %u MiB", (unsigned long long)d->frames, 600 / s,
            double(d->draws - last_draws) / 600, heap_used() >> 20);
        first = false;  // the first interval starts at an arbitrary point
        last = now;
        last_draws = d->draws;
    }
    if (!getenv("RECOMP_NOVSYNC")) wait_vsync();
    if (window.on_idle) window.on_idle(c);
    return D3D_OK;
}

static const Method device_vt[] = {
    COM_IUNKNOWN,
    I(TestCooperativeLevel, 1, ok),
    I(GetAvailableTextureMem, 1, [](CPU *) { return 256u << 20; }),
    I(EvictManagedResources, 1, ok),
    I(GetDirect3D, 2, [](CPU *c) {
        auto d = dev(c);
        add_ref(d->d3d);
        st32(arg(c, 1), d->d3d);
        return D3D_OK;
    }),
    I(GetDeviceCaps, 2, [](CPU *c) {
        fill_caps(arg(c, 1));
        return D3D_OK;
    }),
    I(GetDisplayMode, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t m = arg(c, 2);
        st32(m, d->width);
        st32(m + 4, d->height);
        st32(m + 8, 60);
        st32(m + 12, FMT_X8R8G8B8);
        return D3D_OK;
    }),
    M(GetCreationParameters, 2), M(SetCursorProperties, 4), M(SetCursorPosition, 4),
    I(ShowCursor, 2, zero),
    M(CreateAdditionalSwapChain, 3), M(GetSwapChain, 3), I(GetNumberOfSwapChains, 1, [](CPU *) { return 1u; }),
    I(Reset, 2, ok),
    I(Present, 5, present),
    I(GetBackBuffer, 5, [](CPU *c) {
        auto d = dev(c);
        add_ref(d->backbuffer);
        st32(arg(c, 4), d->backbuffer);
        return D3D_OK;
    }),
    M(GetRasterStatus, 3), M(SetDialogBoxMode, 2),
    I(SetGammaRamp, 4, zero), M(GetGammaRamp, 3),
    I(CreateTexture, 9, [](CPU *c) {
        st32(arg(c, 7), create_texture(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6)));
        return D3D_OK;
    }),
    M(CreateVolumeTexture, 10), M(CreateCubeTexture, 8),
    I(CreateVertexBuffer, 7, [](CPU *c) {
        st32(arg(c, 5), create_buffer(arg(c, 0), 6, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
        return D3D_OK;
    }),
    I(CreateIndexBuffer, 7, [](CPU *c) {
        st32(arg(c, 5), create_buffer(arg(c, 0), 7, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
        return D3D_OK;
    }),
    I(CreateRenderTarget, 9, [](CPU *c) {
        st32(arg(c, 7), new_surface(arg(c, 0), make_image(arg(c, 1), arg(c, 2), arg(c, 3)), 1, 0));
        return D3D_OK;
    }),
    I(CreateDepthStencilSurface, 9, [](CPU *c) {
        st32(arg(c, 7), new_surface(arg(c, 0), make_image(arg(c, 1), arg(c, 2), arg(c, 3)), 2, 0));
        return D3D_OK;
    }),
    M(UpdateSurface, 5), M(UpdateTexture, 3), M(GetRenderTargetData, 3), M(GetFrontBufferData, 3),
    M(StretchRect, 6), M(ColorFill, 4),
    I(CreateOffscreenPlainSurface, 7, [](CPU *c) {
        st32(arg(c, 5), new_surface(arg(c, 0), make_image(arg(c, 1), arg(c, 2), arg(c, 3)), 0, arg(c, 4)));
        return D3D_OK;
    }),
    I(SetRenderTarget, 3, [](CPU *c) {
        auto d = dev(c);
        if (arg(c, 1) != 0) return D3DERR_INVALIDCALL;
        bind(d->st.render_target, arg(c, 2));
        return D3D_OK;
    }),
    I(GetRenderTarget, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t rt = arg(c, 1) == 0 ? d->st.render_target : 0;
        if (rt) add_ref(rt);
        st32(arg(c, 2), rt);
        return rt ? D3D_OK : D3DERR_NOTAVAILABLE;
    }),
    I(SetDepthStencilSurface, 2, [](CPU *c) {
        bind(dev(c)->st.depth, arg(c, 1));
        return D3D_OK;
    }),
    I(GetDepthStencilSurface, 2, [](CPU *c) {
        auto d = dev(c);
        if (d->st.depth) add_ref(d->st.depth);
        st32(arg(c, 1), d->st.depth);
        return d->st.depth ? D3D_OK : D3DERR_NOTAVAILABLE;
    }),
    I(BeginScene, 1, ok), I(EndScene, 1, ok),
    I(Clear, 7, [](CPU *c) {
        auto d = dev(c);
        renderer().clear(*d, arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), f32(arg(c, 5)), arg(c, 6));
        return D3D_OK;
    }),
    I(SetTransform, 3, [](CPU *c) {
        uint32_t s = arg(c, 1);
        if (s < 512) dev(c)->st.transforms[s] = load_matrix(arg(c, 2));
        return D3D_OK;
    }),
    I(GetTransform, 3, [](CPU *c) {
        uint32_t s = arg(c, 1);
        if (s < 512) memcpy(GM + arg(c, 2), dev(c)->st.transforms[s].m, 64);
        return D3D_OK;
    }),
    M(MultiplyTransform, 3),
    I(SetViewport, 2, [](CPU *c) {
        memcpy(dev(c)->st.viewport, GM + arg(c, 1), 24);
        return D3D_OK;
    }),
    I(GetViewport, 2, [](CPU *c) {
        memcpy(GM + arg(c, 1), dev(c)->st.viewport, 24);
        return D3D_OK;
    }),
    I(SetMaterial, 2, [](CPU *c) {
        memcpy(dev(c)->st.material, GM + arg(c, 1), 68);
        return D3D_OK;
    }),
    I(GetMaterial, 2, [](CPU *c) {
        memcpy(GM + arg(c, 1), dev(c)->st.material, 68);
        return D3D_OK;
    }),
    I(SetLight, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t i = arg(c, 1);
        if (i >= 1024) return D3DERR_INVALIDCALL;
        if (d->st.lights.size() <= i) d->st.lights.resize(i + 1);
        memcpy(d->st.lights[i].first.raw, GM + arg(c, 2), 104);
        return D3D_OK;
    }),
    I(GetLight, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t i = arg(c, 1);
        if (i >= d->st.lights.size()) return D3DERR_INVALIDCALL;
        memcpy(GM + arg(c, 2), d->st.lights[i].first.raw, 104);
        return D3D_OK;
    }),
    I(LightEnable, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t i = arg(c, 1);
        if (i >= 1024) return D3DERR_INVALIDCALL;
        if (d->st.lights.size() <= i) {
            d->st.lights.resize(i + 1);
            // A light enabled before being set is the default directional light.
            Light &l = d->st.lights[i].first;
            memset(l.raw, 0, sizeof l.raw);
            l.raw[0] = 3;
            l.raw[1] = l.raw[2] = l.raw[3] = fbits(1.0f);
            l.raw[18] = fbits(1.0f);
        }
        d->st.lights[i].second = arg(c, 2) != 0;
        return D3D_OK;
    }),
    I(GetLightEnable, 3, [](CPU *c) {
        auto d = dev(c);
        uint32_t i = arg(c, 1);
        st32(arg(c, 2), i < d->st.lights.size() && d->st.lights[i].second ? 1 : 0);
        return D3D_OK;
    }),
    M(SetClipPlane, 3), M(GetClipPlane, 3),
    I(SetRenderState, 3, [](CPU *c) {
        if (arg(c, 1) < 256) dev(c)->st.rs[arg(c, 1)] = arg(c, 2);
        return D3D_OK;
    }),
    I(GetRenderState, 3, [](CPU *c) {
        st32(arg(c, 2), arg(c, 1) < 256 ? dev(c)->st.rs[arg(c, 1)] : 0);
        return D3D_OK;
    }),
    I(CreateStateBlock, 3, [](CPU *c) {
        auto d = dev(c);
        st32(arg(c, 2), new_state_block(d->self, d->st));  // every field (all types alike)
        return D3D_OK;
    }),
    I(BeginStateBlock, 1, [](CPU *c) {
        auto d = dev(c);
        if (d->recording) return D3DERR_INVALIDCALL;
        d->recording = std::make_unique<State>(d->st);
        fill_unset(d->st);
        d->st.render_target = d->recording->render_target;
        d->st.depth = d->recording->depth;
        return D3D_OK;
    }),
    I(EndStateBlock, 2, [](CPU *c) {
        auto d = dev(c);
        if (!d->recording) return D3DERR_INVALIDCALL;
        State rec = d->st;
        d->st = *d->recording;
        d->recording.reset();
        st32(arg(c, 1), new_state_block(d->self, rec));
        block_objects(rec, release);  // the references bind() took while recording
        return D3D_OK;
    }), M(SetClipStatus, 2), M(GetClipStatus, 2),
    I(GetTexture, 3, [](CPU *c) {
        uint32_t s = arg(c, 1);
        uint32_t t = s < 16 ? dev(c)->st.textures[s] : 0;
        if (t) add_ref(t);
        st32(arg(c, 2), t);
        return D3D_OK;
    }),
    I(SetTexture, 3, [](CPU *c) {
        uint32_t s = arg(c, 1);
        if (s < 16) bind(dev(c)->st.textures[s], arg(c, 2));
        return D3D_OK;
    }),
    I(GetTextureStageState, 4, [](CPU *c) {
        st32(arg(c, 3), arg(c, 1) < 8 && arg(c, 2) < 33 ? dev(c)->st.tss[arg(c, 1)][arg(c, 2)] : 0);
        return D3D_OK;
    }),
    I(SetTextureStageState, 4, [](CPU *c) {
        if (arg(c, 1) < 8 && arg(c, 2) < 33) dev(c)->st.tss[arg(c, 1)][arg(c, 2)] = arg(c, 3);
        return D3D_OK;
    }),
    I(GetSamplerState, 4, [](CPU *c) {
        st32(arg(c, 3), arg(c, 1) < 16 && arg(c, 2) < 14 ? dev(c)->st.samp[arg(c, 1)][arg(c, 2)] : 0);
        return D3D_OK;
    }),
    I(SetSamplerState, 4, [](CPU *c) {
        if (arg(c, 1) < 16 && arg(c, 2) < 14) dev(c)->st.samp[arg(c, 1)][arg(c, 2)] = arg(c, 3);
        return D3D_OK;
    }),
    I(ValidateDevice, 2, [](CPU *c) {
        if (arg(c, 1)) st32(arg(c, 1), 1);
        return D3D_OK;
    }),
    M(SetPaletteEntries, 3), M(GetPaletteEntries, 3), M(SetCurrentTexturePalette, 2), M(GetCurrentTexturePalette, 2),
    I(SetScissorRect, 2, [](CPU *c) {
        memcpy(dev(c)->st.scissor, GM + arg(c, 1), 16);
        return D3D_OK;
    }),
    I(GetScissorRect, 2, [](CPU *c) {
        memcpy(GM + arg(c, 1), dev(c)->st.scissor, 16);
        return D3D_OK;
    }),
    I(SetSoftwareVertexProcessing, 2, ok), I(GetSoftwareVertexProcessing, 1, zero),
    I(SetNPatchMode, 2, ok), I(GetNPatchMode, 1, zero),
    I(DrawPrimitive, 4, [](CPU *c) {
        Draw dr;
        dr.prim = arg(c, 1);
        dr.start = arg(c, 2);
        dr.count = arg(c, 3);
        return draw(c, dr);
    }),
    I(DrawIndexedPrimitive, 7, [](CPU *c) {
        Draw dr;
        dr.indexed = true;
        dr.prim = arg(c, 1);
        dr.base_vertex = int32_t(arg(c, 2));
        dr.min_index = arg(c, 3);
        dr.num_vertices = arg(c, 4);
        dr.start = arg(c, 5);
        dr.count = arg(c, 6);
        return draw(c, dr);
    }),
    I(DrawPrimitiveUP, 5, [](CPU *c) {
        Draw dr;
        dr.prim = arg(c, 1);
        dr.count = arg(c, 2);
        dr.up_vertices = arg(c, 3);
        dr.up_stride = arg(c, 4);
        uint32_t r = draw(c, dr);
        auto d = dev(c);  // D3D unbinds stream 0 and the indices after a UP draw
        bind(d->st.streams[0].vb, 0);
        d->st.streams[0].offset = d->st.streams[0].stride = 0;
        return r;
    }),
    I(DrawIndexedPrimitiveUP, 9, [](CPU *c) {
        Draw dr;
        dr.indexed = true;
        dr.prim = arg(c, 1);
        dr.min_index = arg(c, 2);
        dr.num_vertices = arg(c, 3);
        dr.count = arg(c, 4);
        dr.up_indices = arg(c, 5);
        dr.up_index_format = arg(c, 6);
        dr.up_vertices = arg(c, 7);
        dr.up_stride = arg(c, 8);
        uint32_t r = draw(c, dr);
        auto d = dev(c);
        bind(d->st.streams[0].vb, 0);
        d->st.streams[0].offset = d->st.streams[0].stride = 0;
        bind(d->st.indices, 0);
        return r;
    }),
    M(ProcessVertices, 7),
    I(CreateVertexDeclaration, 3, [](CPU *c) {
        auto d = std::make_shared<VertexDecl>();
        d->device = arg(c, 0);
        for (uint32_t a = arg(c, 1);; a += 8) {
            VertexElement e{uint16_t(ld16(a)), uint16_t(ld16(a + 2)), uint8_t(ld8(a + 4)), uint8_t(ld8(a + 5)),
                            uint8_t(ld8(a + 6)), uint8_t(ld8(a + 7))};
            if (e.stream == 0xFF) break;
            d->elements.push_back(e);
        }
        st32(arg(c, 2), com_new(d, com_vtable("IDirect3DVertexDeclaration9", decl_vt)));
        return D3D_OK;
    }),
    I(SetVertexDeclaration, 2, [](CPU *c) {
        auto d = dev(c);
        bind(d->st.decl, arg(c, 1));
        if (arg(c, 1)) d->st.fvf = 0;
        return D3D_OK;
    }),
    I(GetVertexDeclaration, 2, [](CPU *c) {
        auto d = dev(c);
        if (d->st.decl) add_ref(d->st.decl);
        st32(arg(c, 1), d->st.decl);
        return D3D_OK;
    }),
    I(SetFVF, 2, [](CPU *c) {
        auto d = dev(c);
        d->st.fvf = arg(c, 1);
        if (arg(c, 1)) bind(d->st.decl, 0);
        return D3D_OK;
    }),
    I(GetFVF, 2, [](CPU *c) {
        st32(arg(c, 1), dev(c)->st.fvf);
        return D3D_OK;
    }),
    I(CreateVertexShader, 3, [](CPU *c) {
        auto s = std::make_shared<Shader>();
        s->device = arg(c, 0);
        uint32_t n = shader_length(arg(c, 1));
        s->code.resize(n);
        memcpy(s->code.data(), GM + arg(c, 1), 4 * n);
        if (trace_imports) log("  vertex shader %08x, %u tokens", s->code[0], n);
        dump_msl(*s);
        st32(arg(c, 2), com_new(s, com_vtable("IDirect3DVertexShader9", shader_vt)));
        return D3D_OK;
    }),
    I(SetVertexShader, 2, [](CPU *c) {
        bind(dev(c)->st.vs, arg(c, 1));
        return D3D_OK;
    }),
    I(GetVertexShader, 2, [](CPU *c) {
        auto d = dev(c);
        if (d->st.vs) add_ref(d->st.vs);
        st32(arg(c, 1), d->st.vs);
        return D3D_OK;
    }),
    I(SetVertexShaderConstantF, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 256) return D3DERR_INVALIDCALL;
        memcpy(&d->st.vs_const[4 * start], GM + arg(c, 2), 16 * n);
        return D3D_OK;
    }),
    I(GetVertexShaderConstantF, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 256) return D3DERR_INVALIDCALL;
        memcpy(GM + arg(c, 2), &d->st.vs_const[4 * start], 16 * n);
        return D3D_OK;
    }),
    I(SetVertexShaderConstantI, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 16) return D3DERR_INVALIDCALL;
        memcpy(&d->st.vs_const_i[4 * start], GM + arg(c, 2), 16 * n);
        return D3D_OK;
    }),
    M(GetVertexShaderConstantI, 4),
    I(SetVertexShaderConstantB, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 16) return D3DERR_INVALIDCALL;
        memcpy(&d->st.vs_const_b[start], GM + arg(c, 2), 4 * n);
        return D3D_OK;
    }),
    M(GetVertexShaderConstantB, 4),
    I(SetStreamSource, 5, [](CPU *c) {
        auto d = dev(c);
        uint32_t s = arg(c, 1);
        if (s >= 16) return D3DERR_INVALIDCALL;
        bind(d->st.streams[s].vb, arg(c, 2));
        d->st.streams[s].offset = arg(c, 3);
        d->st.streams[s].stride = arg(c, 4);
        return D3D_OK;
    }),
    I(GetStreamSource, 5, [](CPU *c) {
        auto d = dev(c);
        uint32_t s = arg(c, 1);
        if (s >= 16) return D3DERR_INVALIDCALL;
        if (d->st.streams[s].vb) add_ref(d->st.streams[s].vb);
        st32(arg(c, 2), d->st.streams[s].vb);
        st32(arg(c, 3), d->st.streams[s].offset);
        st32(arg(c, 4), d->st.streams[s].stride);
        return D3D_OK;
    }),
    I(SetStreamSourceFreq, 3, ok), M(GetStreamSourceFreq, 3),
    I(SetIndices, 2, [](CPU *c) {
        bind(dev(c)->st.indices, arg(c, 1));
        return D3D_OK;
    }),
    I(GetIndices, 2, [](CPU *c) {
        auto d = dev(c);
        if (d->st.indices) add_ref(d->st.indices);
        st32(arg(c, 1), d->st.indices);
        return D3D_OK;
    }),
    I(CreatePixelShader, 3, [](CPU *c) {
        auto s = std::make_shared<Shader>();
        s->device = arg(c, 0);
        s->pixel = true;
        uint32_t n = shader_length(arg(c, 1));
        s->code.resize(n);
        memcpy(s->code.data(), GM + arg(c, 1), 4 * n);
        if (trace_imports) log("  pixel shader %08x, %u tokens", s->code[0], n);
        dump_msl(*s);
        st32(arg(c, 2), com_new(s, com_vtable("IDirect3DPixelShader9", shader_vt)));
        return D3D_OK;
    }),
    I(SetPixelShader, 2, [](CPU *c) {
        bind(dev(c)->st.ps, arg(c, 1));
        return D3D_OK;
    }),
    I(GetPixelShader, 2, [](CPU *c) {
        auto d = dev(c);
        if (d->st.ps) add_ref(d->st.ps);
        st32(arg(c, 1), d->st.ps);
        return D3D_OK;
    }),
    I(SetPixelShaderConstantF, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 32) return D3DERR_INVALIDCALL;
        memcpy(&d->st.ps_const[4 * start], GM + arg(c, 2), 16 * n);
        return D3D_OK;
    }),
    I(GetPixelShaderConstantF, 4, [](CPU *c) {
        auto d = dev(c);
        uint32_t start = arg(c, 1), n = arg(c, 3);
        if (start + n > 32) return D3DERR_INVALIDCALL;
        memcpy(GM + arg(c, 2), &d->st.ps_const[4 * start], 16 * n);
        return D3D_OK;
    }),
    I(SetPixelShaderConstantI, 4, ok), M(GetPixelShaderConstantI, 4),
    I(SetPixelShaderConstantB, 4, ok), M(GetPixelShaderConstantB, 4),
    M(DrawRectPatch, 4), M(DrawTriPatch, 4), M(DeletePatch, 2),
    I(CreateQuery, 3, [](CPU *) { return D3DERR_NOTAVAILABLE; }),
};
static_assert(sizeof(device_vt) / sizeof(device_vt[0]) == 119, "IDirect3DDevice9 has 119 methods");

// ---- IDirect3D9 ----
struct D3D9 : ComObject {};

static void write_mode(uint32_t m) {
    st32(m, 1024);
    st32(m + 4, 768);
    st32(m + 8, 60);
    st32(m + 12, FMT_X8R8G8B8);
}

static const uint32_t kModes[][2] = {{640, 480}, {800, 600}, {1024, 768}};

static const Method d3d9_vt[] = {
    COM_IUNKNOWN,
    M(RegisterSoftwareDevice, 2),
    I(GetAdapterCount, 1, [](CPU *) { return 1u; }),
    I(GetAdapterIdentifier, 4, [](CPU *c) {
        uint32_t p = arg(c, 3);
        memset(GM + p, 0, 1100);
        strcpy(gptr(p), "recomp");
        strcpy(gptr(p + 512), "Daybreak recompiled renderer");
        strcpy(gptr(p + 1024), "\\\\.\\DISPLAY1");
        st32(p + 1064, 0x10DE);  // a vendor the game's checks know
        st32(p + 1068, 0x0322);
        return D3D_OK;
    }),
    I(GetAdapterModeCount, 3, [](CPU *c) { return arg(c, 2) == FMT_X8R8G8B8 ? 3u : 0u; }),
    I(EnumAdapterModes, 5, [](CPU *c) {
        uint32_t i = arg(c, 3);
        if (i >= 3) return D3DERR_INVALIDCALL;
        uint32_t m = arg(c, 4);
        st32(m, kModes[i][0]);
        st32(m + 4, kModes[i][1]);
        st32(m + 8, 60);
        st32(m + 12, arg(c, 2));
        return D3D_OK;
    }),
    I(GetAdapterDisplayMode, 3, [](CPU *c) {
        write_mode(arg(c, 2));
        return D3D_OK;
    }),
    I(CheckDeviceType, 6, ok),
    I(CheckDeviceFormat, 7, [](CPU *c) {
        uint32_t f = arg(c, 6);
        switch (f) {
        case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
        case FMT_A4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_D16: case FMT_D24S8: case FMT_D24X8:
        case FMT_D32: case FMT_DXT1: case FMT_DXT3: case FMT_DXT5:
            return D3D_OK;
        default:
            return D3DERR_NOTAVAILABLE;
        }
    }),
    I(CheckDeviceMultiSampleType, 7, [](CPU *c) {
        if (arg(c, 6)) st32(arg(c, 6), 1);
        return arg(c, 5) == 0 ? D3D_OK : D3DERR_NOTAVAILABLE;
    }),
    I(CheckDepthStencilMatch, 6, ok),
    I(CheckDeviceFormatConversion, 5, ok),
    I(GetDeviceCaps, 4, [](CPU *c) {
        fill_caps(arg(c, 3));
        return D3D_OK;
    }),
    I(GetAdapterMonitor, 2, [](CPU *) { return 0x60001u; }),
    I(CreateDevice, 7, [](CPU *c) {
        uint32_t pp = arg(c, 5);
        auto d = std::make_shared<Device>();
        d->d3d = arg(c, 0);
        add_ref(d->d3d);
        d->width = ld32(pp) ? ld32(pp) : window.width;
        d->height = ld32(pp + 4) ? ld32(pp + 4) : window.height;
        d->backbuffer_format = ld32(pp + 8) ? ld32(pp + 8) : FMT_X8R8G8B8;
        st32(pp, d->width);
        st32(pp + 4, d->height);
        st32(pp + 8, d->backbuffer_format);
        if (!ld32(pp + 12)) st32(pp + 12, 1);
        default_state(d->st);
        d->st.viewport[2] = d->width;
        d->st.viewport[3] = d->height;
        d->st.viewport[5] = fbits(1.0f);
        d->st.scissor[2] = d->width;
        d->st.scissor[3] = d->height;
        d->iids = {"{D0223B96-BF7A-43FD-92BD-A43B0D82B9EB}"};
        uint32_t self = com_new(d, com_vtable("IDirect3DDevice9", device_vt));
        d->backbuffer = new_surface(self, make_image(d->width, d->height, d->backbuffer_format), 1, 0);
        d->st.render_target = d->backbuffer;
        add_ref(d->backbuffer);
        if (ld32(pp + 36)) {
            d->depthbuffer = new_surface(self, make_image(d->width, d->height, ld32(pp + 40)), 2, 0);
            d->st.depth = d->depthbuffer;
            add_ref(d->depthbuffer);
        } else {
            d->st.rs[7] = 0;
        }
        // Like Direct3D 9: unless D3DCREATE_FPU_PRESERVE, the calling thread's x87 switches to
        // single precision (with exceptions masked) and stays so.
        if (!(arg(c, 4) & 2)) c->fcw = uint16_t((c->fcw & ~0x0300u) | 0x003F);
        log("recomp: Direct3D device %ux%u, format %u, depth %u (flags %x)", d->width, d->height,
            d->backbuffer_format, ld32(pp + 36) ? ld32(pp + 40) : 0, arg(c, 4));
        st32(arg(c, 6), self);
        return D3D_OK;
    }),
};

}  // namespace d3d
}  // namespace rt

using namespace rt;

SHIM(d3d9, Direct3DCreate9, 4) {
    auto o = std::make_shared<d3d::D3D9>();
    o->iids = {"{81BDCBCA-64D4-426D-AE8D-AD0147F4275C}"};
    return com_new(o, com_vtable("IDirect3D9", d3d::d3d9_vt));
}
