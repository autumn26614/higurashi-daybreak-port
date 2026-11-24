// Direct3D 9 for the recompiled game: COM objects over guest memory, the device's pipeline
// state, and the interface renderers implement.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "win.h"

namespace rt {
namespace d3d {

enum Format : uint32_t {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_A8B8G8R8 = 32, FMT_X8B8G8R8 = 33, FMT_P8 = 41,
    FMT_L8 = 50, FMT_A8L8 = 51, FMT_D32 = 71, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D16 = 80, FMT_INDEX16 = 101,
    FMT_INDEX32 = 102, FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844,
    FMT_DXT4 = 0x34545844, FMT_DXT5 = 0x35545844,
};
bool is_dxt(uint32_t f);
uint32_t bits_per_pixel(uint32_t f);
uint32_t pitch_of(uint32_t f, uint32_t w);   // bytes per row (per block row for DXT)
uint32_t rows_of(uint32_t f, uint32_t h);    // rows (block rows for DXT)

// A block of pixels in guest memory: a texture level, or a standalone surface.
struct Image {
    uint32_t width = 0, height = 0, format = 0, pitch = 0;
    uint32_t mem = 0, size = 0;  // guest address and size
    uint32_t version = 0;        // bumped on every unlock, so renderers can re-upload
};

struct DeviceChild : ComObject {
    uint32_t device = 0;  // guest device object
};

struct Resource : DeviceChild {
    uint32_t type = 0;  // D3DRESOURCETYPE: 1 surface, 3 texture, 6 vertex buffer, 7 index buffer
    uint32_t pool = 0, usage = 0;
};

struct Texture;
struct Surface : Resource {
    Image own;                // render targets, depth buffers, offscreen surfaces
    Texture *parent = nullptr;  // or a level of this texture
    uint32_t parent_self = 0, level = 0;
    Image &image();
    ~Surface() override;
};

struct Texture : Resource {
    std::vector<Image> levels;
    std::vector<uint32_t> surfaces;  // guest objects for GetSurfaceLevel, made on demand
    ~Texture() override;
};

struct Buffer : Resource {  // vertex or index buffer
    uint32_t length = 0, fvf = 0, format = 0;
    uint32_t mem = 0;
    uint32_t version = 0;
    ~Buffer() override;
};

struct VertexElement {
    uint16_t stream, offset;
    uint8_t type, method, usage, usage_index;
};
struct VertexDecl : DeviceChild {
    std::vector<VertexElement> elements;  // without the END marker
};

struct Shader : DeviceChild {
    std::vector<uint32_t> code;
    bool pixel = false;
};

struct Matrix {
    float m[16];
};

struct Light {
    uint32_t raw[26];  // D3DLIGHT9 as the game wrote it (104 bytes)
};

struct Stream {
    uint32_t vb = 0, offset = 0, stride = 0;
};

struct State {
    std::array<uint32_t, 256> rs{};                  // render states
    std::array<std::array<uint32_t, 33>, 8> tss{};   // texture stage states
    std::array<std::array<uint32_t, 14>, 16> samp{};  // sampler states
    std::array<uint32_t, 16> textures{};             // guest objects
    std::array<Matrix, 512> transforms{};            // view 2, projection 3, texture 16.., world 256..
    uint32_t viewport[6] = {0, 0, 800, 600, 0, 0};    // X Y W H, MinZ MaxZ (floats)
    uint32_t material[17] = {};
    std::vector<std::pair<Light, bool>> lights;      // light, enabled
    std::array<Stream, 16> streams{};
    uint32_t indices = 0, fvf = 0, decl = 0, vs = 0, ps = 0;
    std::vector<float> vs_const = std::vector<float>(256 * 4), ps_const = std::vector<float>(32 * 4);
    std::array<int32_t, 16 * 4> vs_const_i{};
    std::array<uint32_t, 16> vs_const_b{};
    uint32_t scissor[4] = {0, 0, 800, 600};
    uint32_t render_target = 0, depth = 0;
};

// One draw call: the pipeline state at the time and where the data is.
struct Draw {
    uint32_t prim = 0;          // D3DPRIMITIVETYPE
    uint32_t count = 0;         // primitives
    bool indexed = false;
    int32_t base_vertex = 0;
    uint32_t start = 0;         // first vertex, or first index
    uint32_t min_index = 0, num_vertices = 0;
    // User pointer draws (DrawPrimitiveUP): vertex and index data in guest memory.
    uint32_t up_vertices = 0, up_stride = 0, up_indices = 0, up_index_format = 0;
};

struct Device;
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void clear(Device &, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    virtual void draw(Device &, const Draw &) = 0;
    virtual void present(Device &) = 0;
};
void set_renderer(std::unique_ptr<Renderer> r);  // before the game creates its device

struct Device : ComObject {
    uint32_t d3d = 0;
    uint32_t width = 800, height = 600, backbuffer_format = FMT_X8R8G8B8;
    uint32_t backbuffer = 0, depthbuffer = 0;  // guest surfaces
    State st;
    uint64_t frames = 0, draws = 0;
    // State block recording (BeginStateBlock .. EndStateBlock) is done by capturing all.
    bool recording = false;
    template <class T> std::shared_ptr<T> get(uint32_t obj) const { return com_as<T>(obj); }
};

}  // namespace d3d
}  // namespace rt
