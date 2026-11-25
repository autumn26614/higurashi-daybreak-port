// The Metal renderer. Vertex buffers are converted once per version into float4 attributes,
// textures uploaded once per version (render targets stay on the GPU), pipelines and
// samplers cached. Fixed function uses the uber shaders from msl.cpp; the game's shaders
// are translated and compiled on first use.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "metal.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "movie.h"
#include "msl.h"
#include "shader.h"
#include "texconv.h"

namespace rt {
namespace d3d {
namespace {

float f32(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
void color4(uint32_t argb, float *o) {
    o[0] = ((argb >> 16) & 0xFF) / 255.0f;
    o[1] = ((argb >> 8) & 0xFF) / 255.0f;
    o[2] = (argb & 0xFF) / 255.0f;
    o[3] = ((argb >> 24) & 0xFF) / 255.0f;
}

// Mirrors of the MSL blocks (16-byte members only).
struct DrawBlockC {
    float viewport[4], fog[4], fog_color[4], tfactor[4], alpha[4];
    uint32_t flags[4];
    uint32_t stage[8][4];
    uint32_t stage2[8][4];
    float stage_const[8][4];
};
struct LightC {
    float diffuse[4], specular[4], ambient[4], position[4], direction[4], att[4], att2[4];
};
struct FFBlockC {
    float world[4][16];
    float view[16], proj[16];
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4];
    float global_ambient[4], misc[4];
    uint32_t flags[4], sources[4], fogv[4], counts[4];
    LightC lights[8];
};

struct Element {
    uint32_t stream, offset, type, usage, index;
};

std::vector<Element> elements_of(const State &st) {
    std::vector<Element> el;
    if (st.decl) {
        if (auto decl = com_as<VertexDecl>(st.decl))
            for (auto &e : decl->elements) el.push_back({e.stream, e.offset, e.type, e.usage, e.usage_index});
        return el;
    }
    uint32_t fvf = st.fvf, off = 0;
    uint32_t posk = fvf & 0x400E;
    if (posk == 0x4) {
        el.push_back({0, off, 3, 9, 0});
        off += 16;
    } else if (posk) {
        el.push_back({0, off, posk == 0x4002 ? 3u : 2u, 0, 0});
        off += posk == 0x4002 ? 16 : 12;
        int betas = posk >= 6 && posk <= 0xE ? int((posk - 4) / 2) : 0;
        if (betas) {
            bool last_idx = (fvf & 0x1000) || (fvf & 0x8000);
            int w = last_idx ? betas - 1 : betas;
            if (w > 0) el.push_back({0, off, uint32_t(w - 1), 1, 0});
            off += 4 * uint32_t(w);
            if (last_idx) {
                el.push_back({0, off, (fvf & 0x8000) ? 4u : 5u, 2, 0});
                off += 4;
            }
        }
    }
    if (fvf & 0x10) { el.push_back({0, off, 2, 3, 0}); off += 12; }
    if (fvf & 0x20) { el.push_back({0, off, 0, 4, 0}); off += 4; }
    if (fvf & 0x40) { el.push_back({0, off, 4, 10, 0}); off += 4; }
    if (fvf & 0x80) { el.push_back({0, off, 4, 10, 1}); off += 4; }
    uint32_t ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        uint32_t n = f == 0 ? 2 : f == 1 ? 3 : f == 2 ? 4 : 1;
        el.push_back({0, off, n - 1, 5, i});
        off += 4 * n;
    }
    return el;
}

void fetch(const uint8_t *p, uint32_t type, float *v) {
    v[0] = v[1] = v[2] = 0;
    v[3] = 1;
    switch (type) {
    case 0: case 1: case 2: case 3: memcpy(v, p, 4 * (type + 1)); break;
    case 4: v[0] = p[2] / 255.0f; v[1] = p[1] / 255.0f; v[2] = p[0] / 255.0f; v[3] = p[3] / 255.0f; break;
    case 5: for (int i = 0; i < 4; i++) v[i] = p[i]; break;
    case 6: case 7: {
        int16_t s[4] = {0, 0, 0, 1};
        memcpy(s, p, type == 6 ? 4 : 8);
        for (int i = 0; i < 4; i++) v[i] = s[i];
        break;
    }
    case 8: for (int i = 0; i < 4; i++) v[i] = p[i] / 255.0f; break;
    case 9: case 10: {
        int16_t s[4] = {0, 0, 0, 32767};
        memcpy(s, p, type == 9 ? 4 : 8);
        for (int i = 0; i < 4; i++) v[i] = s[i] / 32767.0f;
        break;
    }
    default: break;
    }
}

int ff_slot(uint32_t usage, uint32_t index) {
    switch (usage) {
    case 0: return msl::FF_POSITION;
    case 9: return msl::FF_POSITIONT;
    case 1: return msl::FF_BLENDWEIGHT;
    case 2: return msl::FF_BLENDINDICES;
    case 3: return msl::FF_NORMAL;
    case 10: return index == 0 ? msl::FF_COLOR0 : msl::FF_COLOR1;
    case 5: return index < 8 ? msl::FF_TEXCOORD0 + int(index) : -1;
    case 4: return msl::FF_PSIZE;
    default: return -1;
    }
}

MTLBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation blend_op(uint32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
MTLCompareFunction compare(uint32_t f) {
    switch (f) {
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
MTLSamplerAddressMode address(uint32_t m) {
    switch (m) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToZero;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

uint64_t hash_bytes(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

constexpr uint32_t kFrames = 3;
constexpr size_t kRingSize = 16u << 20;

class MetalRenderer : public Renderer {
public:
    explicit MetalRenderer(CAMetalLayer *layer) : layer_(layer) {
        dev_ = layer.device ? layer.device : MTLCreateSystemDefaultDevice();
        layer.device = dev_;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        queue_ = [dev_ newCommandQueue];
        sem_ = dispatch_semaphore_create(kFrames);
        for (uint32_t i = 0; i < kFrames; i++) ring_[i] = [dev_ newBufferWithLength:kRingSize options:MTLResourceStorageModeShared];
        NSError *err = nil;
        std::string src = msl::prelude() + msl::fixed_function();
        lib_ = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:nil error:&err];
        if (!lib_) log("recomp: Metal: the fixed-function library does not compile: %s", err.localizedDescription.UTF8String);
        float defaults[3][4] = {{0, 0, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}};
        defaults_ = [dev_ newBufferWithBytes:defaults length:sizeof defaults options:MTLResourceStorageModeShared];
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        clear_depth_ = [dev_ newDepthStencilStateWithDescriptor:dd];
        dd.depthWriteEnabled = NO;
        no_depth_ = [dev_ newDepthStencilStateWithDescriptor:dd];
    }

    // ---- Renderer ----
    void clear(Device &d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t) override {
        begin_frame();
        Target t = target(d);
        if (!t.color) return;
        const uint32_t *vp = d.st.viewport;
        bool full = count == 0 && vp[0] == 0 && vp[1] == 0 && vp[2] >= t.color.width && vp[3] >= t.color.height &&
                    !d.st.rs[174];
        if (full) {  // a new pass that starts by clearing
            end_pass();
            pending_ = {flags, color, z};
            have_pending_ = true;
            ensure_pass(d);
            return;
        }
        ensure_pass(d);
        float c[4];
        color4(color, c);
        std::vector<MTLScissorRect> areas;
        if (!count) areas.push_back(clip({int(vp[0]), int(vp[1]), int(vp[0] + vp[2]), int(vp[1] + vp[3])}, t));
        for (uint32_t i = 0; i < count; i++) {
            uint32_t r = rects + 16 * i;
            areas.push_back(clip({int(ld32(r)), int(ld32(r + 4)), int(ld32(r + 8)), int(ld32(r + 12))}, t));
        }
        [enc_ setRenderPipelineState:clear_pipeline((flags & 1) != 0, t)];
        [enc_ setDepthStencilState:(flags & 2) && t.depth ? clear_depth_ : no_depth_];
        [enc_ setCullMode:MTLCullModeNone];
        MTLViewport full_vp = {0, 0, double(t.color.width), double(t.color.height), 0, 1};
        [enc_ setViewport:full_vp];
        float zz[4] = {z, 0, 0, 0};
        [enc_ setVertexBytes:zz length:16 atIndex:0];
        [enc_ setFragmentBytes:c length:16 atIndex:0];
        for (auto &s : areas) {
            if (!s.width || !s.height) continue;
            [enc_ setScissorRect:s];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }
        state_dirty_ = true;
    }

    void draw(Device &d, const Draw &dr) override {
        begin_frame();
        Target t = target(d);
        if (!t.color) return;
        ensure_pass(d);
        const State &st = d.st;
        std::vector<Element> el = elements_of(st);
        auto vs = st.vs ? com_as<Shader>(st.vs) : nullptr;
        auto ps = st.ps ? com_as<Shader>(st.ps) : nullptr;
        id<MTLFunction> vfn = vs ? shader_fn(*vs) : [lib_ newFunctionWithName:@"ff_vertex"];
        id<MTLFunction> ffn = ps ? shader_fn(*ps) : [lib_ newFunctionWithName:@"ff_fragment"];
        if (!vfn || !ffn) return;
        bool pretransformed = false;
        bool has_spec = false;
        for (auto &e : el) {
            if (e.usage == 9) pretransformed = !vs;
            if (e.usage == 10 && e.index == 1) has_spec = true;
        }

        // Vertex input: each stream converted to float4 attributes.
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        std::string sig;
        std::map<uint32_t, std::vector<const Element *>> streams;
        for (auto &e : el) streams[e.stream & 15].push_back(&e);
        std::map<int, std::pair<uint32_t, uint32_t>> attr;  // attribute -> (buffer, offset)
        if (vs) {
            const shader::Program &p = program(*vs);
            for (auto &in : p.inputs) {
                int a = int(in[0]);
                bool found = false;
                for (auto &[s, list] : streams)
                    for (size_t k = 0; k < list.size(); k++)
                        if (list[k]->usage == in[1] && list[k]->index == in[2]) {
                            attr[a] = {s, uint32_t(16 * k)};
                            found = true;
                        }
                if (!found) attr[a] = {16, in[1] == 10 ? (in[2] == 0 ? 16u : 32u) : 0u};
            }
            if (p.inputs.empty()) attr[0] = {16, 0};
        } else {
            for (int slot = 0; slot < msl::FF_SLOTS; slot++)
                attr[slot] = {16, slot == msl::FF_COLOR0 ? 16u : slot == msl::FF_COLOR1 ? 32u : 0u};
            for (auto &[s, list] : streams)
                for (size_t k = 0; k < list.size(); k++) {
                    int slot = ff_slot(list[k]->usage, list[k]->index);
                    if (slot >= 0) attr[slot] = {s, uint32_t(16 * k)};
                }
        }
        for (auto &[a, bo] : attr) {
            vd.attributes[NSUInteger(a)].format = MTLVertexFormatFloat4;
            vd.attributes[NSUInteger(a)].bufferIndex = bo.first;
            vd.attributes[NSUInteger(a)].offset = bo.second;
            sig += std::to_string(a) + ":" + std::to_string(bo.first) + "/" + std::to_string(bo.second) + ",";
        }
        for (auto &[s, list] : streams) {
            vd.layouts[s].stride = 16 * list.size();
            vd.layouts[s].stepFunction = MTLVertexStepFunctionPerVertex;
        }
        vd.layouts[16].stride = 16;
        vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[16].stepRate = 0;

        id<MTLRenderPipelineState> pso = pipeline(d, vfn, ffn, vd, sig, t);
        if (!pso) return;
        [enc_ setRenderPipelineState:pso];
        [enc_ setDepthStencilState:depth_state(st, t)];
        uint32_t cull = st.rs[22];
        [enc_ setFrontFacingWinding:MTLWindingClockwise];
        [enc_ setCullMode:cull == 3 ? MTLCullModeBack : cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        [enc_ setTriangleFillMode:st.rs[8] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        const uint32_t *vp = st.viewport;
        MTLViewport mvp = {double(vp[0]), double(vp[1]), double(vp[2]), double(vp[3]), double(f32(vp[4])), double(f32(vp[5]))};
        [enc_ setViewport:mvp];
        MTLScissorRect sc = st.rs[174] ? clip({int(st.scissor[0]), int(st.scissor[1]), int(st.scissor[2]), int(st.scissor[3])}, t)
                                       : MTLScissorRect{0, 0, t.color.width, t.color.height};
        if (!sc.width || !sc.height) return;
        [enc_ setScissorRect:sc];
        uint32_t bf = st.rs[193];
        [enc_ setBlendColorRed:((bf >> 16) & 0xFF) / 255.0f green:((bf >> 8) & 0xFF) / 255.0f blue:(bf & 0xFF) / 255.0f
                         alpha:((bf >> 24) & 0xFF) / 255.0f];

        // Vertex data.
        uint32_t nverts = 0;
        int64_t base = 0;
        if (dr.up_vertices) {
            for (auto &[s, list] : streams) {
                if (s != 0) continue;
                uint32_t count = dr.indexed ? dr.min_index + dr.num_vertices : vertex_count(dr.prim, dr.count) + dr.start;
                size_t bytes = size_t(count) * list.size() * 16;
                size_t off;
                float *dst = static_cast<float *>(ring_alloc(bytes, off));
                if (!dst) return;
                for (uint32_t v = 0; v < count; v++)
                    for (size_t k = 0; k < list.size(); k++)
                        fetch(GM + dr.up_vertices + v * dr.up_stride + list[k]->offset, list[k]->type, dst + (v * list.size() + k) * 4);
                [enc_ setVertexBuffer:ring_[frame_ % kFrames] offset:off atIndex:0];
            }
        } else {
            base = dr.indexed ? dr.base_vertex : 0;
            for (auto &[s, list] : streams) {
                const Stream &str = st.streams[s];
                auto vb = com_as<Buffer>(str.vb);
                if (!vb || !str.stride) return;
                VBuf &cb = vertex_buffer(*vb, str, list);
                if (!cb.buf) return;
                if (base < 0) return;
                [enc_ setVertexBuffer:cb.buf offset:size_t(base) * list.size() * 16 atIndex:s];
                nverts = std::max(nverts, cb.count);
            }
        }
        [enc_ setVertexBuffer:defaults_ offset:0 atIndex:16];

        // Uniforms.
        DrawBlockC db{};
        draw_block(d, db, pretransformed, vs != nullptr);
        [enc_ setVertexBytes:&db length:sizeof db atIndex:18];
        [enc_ setFragmentBytes:&db length:sizeof db atIndex:1];
        if (vs) {
            std::vector<float> c(st.vs_const.begin(), st.vs_const.end());
            [enc_ setVertexBytes:c.data() length:256 * 16 atIndex:17];
        } else {
            FFBlockC fb{};
            ff_block(d, fb, has_spec);
            [enc_ setVertexBytes:&fb length:sizeof fb atIndex:17];
        }
        if (ps) [enc_ setFragmentBytes:st.ps_const.data() length:32 * 16 atIndex:0];
        for (uint32_t s = 0; s < 8; s++) {
            id<MTLTexture> tx = st.textures[s] ? texture(st.textures[s]) : nil;
            [enc_ setFragmentTexture:tx ? tx : white() atIndex:s];
            [enc_ setFragmentSamplerState:sampler(st.samp[s]) atIndex:s];
        }

        // The draw.
        MTLPrimitiveType prim;
        switch (dr.prim) {
        case 1: prim = MTLPrimitiveTypePoint; break;
        case 2: prim = MTLPrimitiveTypeLine; break;
        case 3: prim = MTLPrimitiveTypeLineStrip; break;
        case 5: prim = MTLPrimitiveTypeTriangleStrip; break;
        default: prim = MTLPrimitiveTypeTriangle; break;
        }
        uint32_t n = vertex_count(dr.prim, dr.count);
        if (!n) return;
        if (dr.prim == 6) {  // fans become lists
            std::vector<uint32_t> idx;
            for (uint32_t i = 1; i + 1 < n; i++) {
                uint32_t a = 0, b = i, c = i + 1;
                if (dr.indexed) {
                    a = index_at(d, dr, 0);
                    b = index_at(d, dr, i);
                    c = index_at(d, dr, i + 1);
                } else {
                    a += dr.start, b += dr.start, c += dr.start;
                }
                idx.insert(idx.end(), {a, b, c});
            }
            size_t off;
            void *p = ring_alloc(idx.size() * 4, off);
            if (!p) return;
            memcpy(p, idx.data(), idx.size() * 4);
            [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:idx.size() indexType:MTLIndexTypeUInt32
                            indexBuffer:ring_[frame_ % kFrames] indexBufferOffset:off];
            return;
        }
        if (!dr.indexed) {
            [enc_ drawPrimitives:prim vertexStart:dr.start vertexCount:n];
            return;
        }
        if (dr.up_indices) {
            uint32_t w = dr.up_index_format == FMT_INDEX32 ? 4 : 2;
            size_t off;
            void *p = ring_alloc(size_t(n) * w + 4, off);
            if (!p) return;
            memcpy(p, GM + dr.up_indices, size_t(n) * w);
            [enc_ drawIndexedPrimitives:prim indexCount:n indexType:w == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                            indexBuffer:ring_[frame_ % kFrames] indexBufferOffset:off];
            return;
        }
        auto ib = com_as<Buffer>(st.indices);
        if (!ib) return;
        IBuf &ibuf = index_buffer(*ib);
        uint32_t w = ib->format == FMT_INDEX32 ? 4 : 2;
        if (uint64_t(dr.start + n) * w > ib->length) return;
        [enc_ drawIndexedPrimitives:prim indexCount:n indexType:w == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                        indexBuffer:ibuf.buf indexBufferOffset:size_t(dr.start) * w];
        (void)nverts;
    }

    void present(Device &d) override {
        begin_frame();
        end_pass();
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        auto bb = com_as<Surface>(d.backbuffer);
        if (drawable && bb) {
            id<MTLTexture> frame = color_target(bb->image());
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cmd_ renderCommandEncoderWithDescriptor:rp];
            double W = drawable.texture.width, H = drawable.texture.height;
            double s = std::min(W / 800.0, H / 600.0);
            double w = 800 * s, h = 600 * s, x = (W - w) / 2, y = (H - h) / 2;
            auto ndc = [&](double px, double py, double pw, double ph, float *r) {  // x0, ytop, x1, ybottom
                r[0] = float((x + px * s) / W * 2 - 1);
                r[1] = float(1 - (y + py * s) / H * 2);
                r[2] = float((x + (px + pw) * s) / W * 2 - 1);
                r[3] = float(1 - (y + (py + ph) * s) / H * 2);
            };
            [e setRenderPipelineState:blit_pipeline()];
            [e setFragmentSamplerState:linear_sampler() atIndex:0];
            float r[4];
            ndc(0, 0, 800, 600, r);
            [e setVertexBytes:r length:16 atIndex:0];
            [e setFragmentTexture:frame atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            if (movie::current(movie_)) {
                if (movie_.serial != movie_serial_ || !movie_tex_ || movie_tex_.width != NSUInteger(movie_.width) ||
                    movie_tex_.height != NSUInteger(movie_.height)) {
                    if (!movie_tex_ || movie_tex_.width != NSUInteger(movie_.width) || movie_tex_.height != NSUInteger(movie_.height)) {
                        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                      width:NSUInteger(movie_.width)
                                                                                                     height:NSUInteger(movie_.height)
                                                                                                  mipmapped:NO];
                        movie_tex_ = [dev_ newTextureWithDescriptor:td];
                    }
                    [movie_tex_ replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(movie_.width), NSUInteger(movie_.height))
                                  mipmapLevel:0
                                    withBytes:movie_.rgba.data()
                                  bytesPerRow:NSUInteger(movie_.width) * 4];
                    movie_serial_ = movie_.serial;
                }
                ndc(movie_.x, movie_.y, movie_.w, movie_.h, r);
                [e setVertexBytes:r length:16 atIndex:0];
                [e setFragmentTexture:movie_tex_ atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
            [e endEncoding];
            [cmd_ presentDrawable:drawable];
        }
        dispatch_semaphore_t sem = sem_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) { dispatch_semaphore_signal(sem); }];
        [cmd_ commit];
        cmd_ = nil;
        frame_++;
    }

    bool read_frame(Device &d, std::vector<uint32_t> &argb, uint32_t &w, uint32_t &h) override {
        auto bb = com_as<Surface>(d.backbuffer);
        if (!bb) return false;
        id<MTLTexture> frame = color_target(bb->image());
        w = uint32_t(frame.width);
        h = uint32_t(frame.height);
        id<MTLBuffer> buf = [dev_ newBufferWithLength:size_t(w) * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:frame sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0
          destinationBytesPerRow:w * 4 destinationBytesPerImage:size_t(w) * h * 4];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        argb.resize(size_t(w) * h);
        memcpy(argb.data(), buf.contents, argb.size() * 4);  // BGRA8 in memory = 0xAARRGGBB words
        return true;
    }

private:
    struct Target {
        id<MTLTexture> color = nil, depth = nil;
    };
    struct Clear {
        uint32_t flags = 0, color = 0;
        float z = 1;
    };
    struct Tex {
        id<MTLTexture> tex = nil;
        uint64_t version = ~0ull;
        bool rt = false;
        uint32_t rt_version = 0;  // the image a render target was made for (versions are unique)
    };
    struct VBuf {
        id<MTLBuffer> buf = nil;
        uint32_t version = ~0u, count = 0;
    };
    struct IBuf {
        id<MTLBuffer> buf = nil;
        uint32_t version = ~0u;
    };

    // ---- frames and passes ----
    void begin_frame() {
        if (cmd_) return;
        dispatch_semaphore_wait(sem_, DISPATCH_TIME_FOREVER);
        cmd_ = [queue_ commandBuffer];
        ring_off_ = 0;
    }

    void end_pass() {
        if (enc_) [enc_ endEncoding];
        enc_ = nil;
        pass_color_ = nil;
        pass_depth_ = nil;
    }

    Target target(Device &d) {
        Target t;
        if (auto s = com_as<Surface>(d.st.render_target)) t.color = color_target(s->image());
        if (auto s = com_as<Surface>(d.st.depth)) {
            Image &im = s->image();
            auto &dt = depth_[im.mem];
            if (!dt || dt.width != im.width || dt.height != im.height) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                              width:im.width
                                                                                             height:im.height
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget;
                td.storageMode = MTLStorageModePrivate;
                dt = [dev_ newTextureWithDescriptor:td];
            }
            if (t.color && dt.width == t.color.width && dt.height == t.color.height) t.depth = dt;
        }
        return t;
    }

    void ensure_pass(Device &d) {
        Target t = target(d);
        if (enc_ && pass_color_ == t.color && pass_depth_ == t.depth) return;
        end_pass();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t.color;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (have_pending_ && (pending_.flags & 1)) {
            float c[4];
            color4(pending_.color, c);
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
        } else {
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        }
        if (t.depth) {
            rp.depthAttachment.texture = t.depth;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (have_pending_ && (pending_.flags & 2)) {
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = pending_.z;
            } else {
                rp.depthAttachment.loadAction = depth_fresh_.count((__bridge void *)t.depth) ? MTLLoadActionLoad : MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
            }
            depth_fresh_[(__bridge void *)t.depth] = true;
        }
        have_pending_ = false;
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
        pass_color_ = t.color;
        pass_depth_ = t.depth;
    }

    MTLScissorRect clip(std::array<int, 4> r, const Target &t) {
        int x0 = std::max(0, r[0]), y0 = std::max(0, r[1]);
        int x1 = std::min(int(t.color.width), r[2]), y1 = std::min(int(t.color.height), r[3]);
        if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
        return {NSUInteger(x0), NSUInteger(y0), NSUInteger(x1 - x0), NSUInteger(y1 - y0)};
    }

    void *ring_alloc(size_t bytes, size_t &off) {
        size_t start = (ring_off_ + 255) & ~size_t(255);
        if (start + bytes > kRingSize) return nullptr;
        ring_off_ = start + bytes;
        off = start;
        return static_cast<uint8_t *>([ring_[frame_ % kFrames] contents]) + start;
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

    uint32_t index_at(Device &d, const Draw &dr, uint32_t i) {
        if (dr.up_indices) return dr.up_index_format == FMT_INDEX32 ? ld32(dr.up_indices + 4 * i) : ld16(dr.up_indices + 2 * i);
        auto ib = com_as<Buffer>(d.st.indices);
        if (!ib) return 0;
        uint32_t w = ib->format == FMT_INDEX32 ? 4 : 2;
        uint32_t v = w == 4 ? ld32(ib->mem + (dr.start + i) * 4) : ld16(ib->mem + (dr.start + i) * 2);
        return uint32_t(int64_t(v) + dr.base_vertex);
    }

    // ---- resources ----
    id<MTLTexture> color_target(Image &im) {
        Tex &t = textures_[im.mem];
        if (!t.tex || !t.rt || t.rt_version != im.version || t.tex.width != im.width || t.tex.height != im.height) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:im.width
                                                                                         height:im.height
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            t.tex = [dev_ newTextureWithDescriptor:td];
            t.rt = true;
            t.rt_version = im.version;
        }
        return t.tex;
    }

    id<MTLTexture> texture(uint32_t obj) {
        auto tx = com_as<Texture>(obj);
        if (!tx || tx->levels.empty()) return nil;
        Image &l0 = tx->levels[0];
        Tex &t = textures_[l0.mem];
        if (t.rt) {
            if (t.rt_version == l0.version) return t.tex;
            t = Tex{};  // freed render target memory now holds another texture
        }
        uint64_t version = 0;
        for (auto &l : tx->levels) version = version * 31 + l.version;
        if (t.tex && t.version == version && t.tex.width == l0.width && t.tex.height == l0.height) return t.tex;
        if (!t.tex || t.tex.width != l0.width || t.tex.height != l0.height || t.tex.mipmapLevelCount != tx->levels.size()) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:l0.width
                                                                                         height:l0.height
                                                                                      mipmapped:tx->levels.size() > 1];
            td.mipmapLevelCount = tx->levels.size();
            t.tex = [dev_ newTextureWithDescriptor:td];
        }
        std::vector<uint32_t> px;
        for (size_t i = 0; i < tx->levels.size(); i++) {
            const Image &im = tx->levels[i];
            decode_bgra8(im, px);
            [t.tex replaceRegion:MTLRegionMake2D(0, 0, im.width, im.height) mipmapLevel:i withBytes:px.data() bytesPerRow:im.width * 4];
        }
        t.version = version;
        return t.tex;
    }

    id<MTLTexture> white() {
        if (!white_) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            white_ = [dev_ newTextureWithDescriptor:td];
            uint32_t px = 0xFF000000;  // an empty stage reads opaque black
            [white_ replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
        }
        return white_;
    }

    VBuf &vertex_buffer(Buffer &vb, const Stream &s, const std::vector<const Element *> &list) {
        std::string key = std::to_string(vb.mem) + "/" + std::to_string(s.offset) + "/" + std::to_string(s.stride);
        for (auto *e : list) key += ":" + std::to_string(e->offset) + "." + std::to_string(e->type);
        VBuf &c = vbufs_[key];
        if (c.buf && c.version == vb.version) return c;
        uint32_t count = vb.length > s.offset ? (vb.length - s.offset) / s.stride : 0;
        if (!count) {
            c.buf = nil;
            return c;
        }
        std::vector<float> out(size_t(count) * list.size() * 4);
        for (uint32_t v = 0; v < count; v++)
            for (size_t k = 0; k < list.size(); k++) {
                uint64_t o = uint64_t(s.offset) + uint64_t(v) * s.stride + list[k]->offset;
                if (o + 4 <= vb.length) fetch(GM + vb.mem + uint32_t(o), list[k]->type, &out[(v * list.size() + k) * 4]);
            }
        c.buf = [dev_ newBufferWithBytes:out.data() length:out.size() * 4 options:MTLResourceStorageModeShared];
        c.version = vb.version;
        c.count = count;
        return c;
    }

    IBuf &index_buffer(Buffer &ib) {
        IBuf &c = ibufs_[ib.mem];
        if (c.buf && c.version == ib.version && c.buf.length >= ib.length) return c;
        c.buf = [dev_ newBufferWithBytes:GM + ib.mem length:std::max<uint32_t>(ib.length, 4) options:MTLResourceStorageModeShared];
        c.version = ib.version;
        return c;
    }

    // ---- state ----
    const shader::Program &program(const Shader &s) {
        uint64_t h = hash_bytes(s.code.data(), s.code.size() * 4);
        auto it = programs_.find(h);
        if (it == programs_.end()) it = programs_.emplace(h, shader::decode(s.code.data(), s.code.size())).first;
        return it->second;
    }

    id<MTLFunction> shader_fn(const Shader &s) {
        uint64_t h = hash_bytes(s.code.data(), s.code.size() * 4);
        auto it = functions_.find(h);
        if (it != functions_.end()) return it->second;
        const shader::Program &p = program(s);
        char name[32];
        snprintf(name, sizeof name, "%s_%016llx", s.pixel ? "ps" : "vs", (unsigned long long)h);
        std::string err;
        std::string fn = s.pixel ? msl::fragment(p, name, &err) : msl::vertex(p, name, &err);
        id<MTLFunction> f = nil;
        if (!fn.empty()) {
            NSError *e = nil;
            std::string src = msl::prelude() + fn;
            id<MTLLibrary> lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:nil error:&e];
            if (lib) f = [lib newFunctionWithName:[NSString stringWithUTF8String:name]];
            else log("recomp: Metal: %s does not compile: %s", name, e.localizedDescription.UTF8String);
        } else {
            log("recomp: Metal: %s does not translate: %s", name, err.c_str());
        }
        functions_[h] = f;
        return f;
    }

    id<MTLRenderPipelineState> pipeline(Device &d, id<MTLFunction> vfn, id<MTLFunction> ffn, MTLVertexDescriptor *vd,
                                        const std::string &sig, const Target &t) {
        const State &st = d.st;
        bool blend = st.rs[27] != 0;
        uint32_t sb = st.rs[19], db = st.rs[20];
        if (sb == 12) { sb = 5; db = 6; }
        if (sb == 13) { sb = 6; db = 5; }
        bool sep = st.rs[206] != 0;
        char key[256];
        snprintf(key, sizeof key, "%p|%p|%d.%u.%u.%u.%d.%u.%u.%u|%x|%d", (__bridge void *)vfn, (__bridge void *)ffn, blend, sb, db,
                 st.rs[171], sep, st.rs[207], st.rs[208], st.rs[209], st.rs[168] & 0xF, t.depth != nil);
        std::string k = std::string(key) + sig;
        auto it = pipelines_.find(k);
        if (it != pipelines_.end()) return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vfn;
        pd.fragmentFunction = ffn;
        pd.vertexDescriptor = vd;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        if (t.depth) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        auto *ca = pd.colorAttachments[0];
        uint32_t mask = st.rs[168];
        MTLColorWriteMask wm = MTLColorWriteMaskNone;
        if (mask & 1) wm |= MTLColorWriteMaskRed;
        if (mask & 2) wm |= MTLColorWriteMaskGreen;
        if (mask & 4) wm |= MTLColorWriteMaskBlue;
        if (mask & 8) wm |= MTLColorWriteMaskAlpha;
        ca.writeMask = wm;
        ca.blendingEnabled = blend;
        if (blend) {
            ca.sourceRGBBlendFactor = blend_factor(sb);
            ca.destinationRGBBlendFactor = blend_factor(db);
            ca.rgbBlendOperation = blend_op(st.rs[171]);
            ca.sourceAlphaBlendFactor = sep ? blend_factor(st.rs[207]) : blend_factor(sb);
            ca.destinationAlphaBlendFactor = sep ? blend_factor(st.rs[208]) : blend_factor(db);
            ca.alphaBlendOperation = sep ? blend_op(st.rs[209]) : blend_op(st.rs[171]);
        }
        NSError *err = nil;
        id<MTLRenderPipelineState> p = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p) log("recomp: Metal pipeline: %s", err.localizedDescription.UTF8String);
        pipelines_[k] = p;
        return p;
    }

    id<MTLRenderPipelineState> clear_pipeline(bool color, const Target &t) {
        std::string k = std::string("clear") + (color ? "c" : "-") + (t.depth ? "d" : "-");
        auto it = pipelines_.find(k);
        if (it != pipelines_.end()) return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib_ newFunctionWithName:@"clear_vs"];
        pd.fragmentFunction = [lib_ newFunctionWithName:@"clear_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].writeMask = color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        if (t.depth) pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *err = nil;
        id<MTLRenderPipelineState> p = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        pipelines_[k] = p;
        return p;
    }

    id<MTLRenderPipelineState> blit_pipeline() {
        if (!blit_) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib_ newFunctionWithName:@"blit_vs"];
            pd.fragmentFunction = [lib_ newFunctionWithName:@"blit_fs"];
            pd.colorAttachments[0].pixelFormat = layer_.pixelFormat;
            NSError *err = nil;
            blit_ = [dev_ newRenderPipelineStateWithDescriptor:pd error:&err];
        }
        return blit_;
    }

    id<MTLSamplerState> linear_sampler() {
        if (!linear_) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            linear_ = [dev_ newSamplerStateWithDescriptor:sd];
        }
        return linear_;
    }

    id<MTLDepthStencilState> depth_state(const State &st, const Target &t) {
        bool on = st.rs[7] != 0 && t.depth;
        uint32_t key = on ? (st.rs[23] << 1 | (st.rs[14] ? 1 : 0)) : 0xFFFF;
        auto &ds = depth_states_[key];
        if (!ds) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = on ? compare(st.rs[23]) : MTLCompareFunctionAlways;
            dd.depthWriteEnabled = on && st.rs[14];
            ds = [dev_ newDepthStencilStateWithDescriptor:dd];
        }
        return ds;
    }

    id<MTLSamplerState> sampler(const std::array<uint32_t, 14> &s) {
        uint64_t key = uint64_t(s[1]) | uint64_t(s[2]) << 4 | uint64_t(s[5]) << 8 | uint64_t(s[6]) << 12 | uint64_t(s[7]) << 16 |
                       uint64_t(std::min<uint32_t>(s[10], 16)) << 20;
        auto &ss = samplers_[key];
        if (!ss) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.sAddressMode = address(s[1]);
            sd.tAddressMode = address(s[2]);
            sd.magFilter = s[5] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.minFilter = s[6] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.mipFilter = s[7] == 0 ? MTLSamplerMipFilterNotMipmapped : s[7] == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
            if (s[5] == 3 || s[6] == 3) sd.maxAnisotropy = std::max<NSUInteger>(1, std::min<NSUInteger>(16, s[10]));
            ss = [dev_ newSamplerStateWithDescriptor:sd];
        }
        return ss;
    }

    void draw_block(Device &d, DrawBlockC &b, bool pretransformed, bool has_vs) {
        const State &st = d.st;
        const uint32_t *vp = st.viewport;
        b.viewport[0] = float(vp[0]);
        b.viewport[1] = float(vp[1]);
        b.viewport[2] = float(std::max(1u, vp[2]));
        b.viewport[3] = float(std::max(1u, vp[3]));
        b.flags[0] = pretransformed;
        if (st.rs[28]) {
            if (st.rs[35]) {
                b.flags[1] = 2;
                b.fog[3] = float(st.rs[35]);
            } else if (st.rs[140] || has_vs || true) {
                b.flags[1] = 1;
            }
        }
        b.fog[0] = f32(st.rs[36]);
        b.fog[1] = f32(st.rs[37]);
        b.fog[2] = f32(st.rs[38]);
        color4(st.rs[34], b.fog_color);
        color4(st.rs[60], b.tfactor);
        b.flags[2] = st.rs[15] != 0;
        b.alpha[0] = float(st.rs[24] & 0xFF);
        b.alpha[1] = float(st.rs[25]);
        b.flags[3] = st.rs[29] != 0;
        for (int s = 0; s < 8; s++) {
            const auto &t = st.tss[s];
            b.stage[s][0] = t[1];
            b.stage[s][1] = t[4];
            b.stage[s][2] = (t[2] & 0xFF) | (t[3] & 0xFF) << 8 | (t[26] & 0xFF) << 16;
            b.stage[s][3] = (t[5] & 0xFF) | (t[6] & 0xFF) << 8 | (t[27] & 0xFF) << 16;
            b.stage2[s][0] = t[28];
            b.stage2[s][1] = st.textures[s] != 0;
            b.stage2[s][2] = has_vs ? (t[11] & 7) : uint32_t(s);
            color4(t[32], b.stage_const[s]);
        }
    }

    void ff_block(Device &d, FFBlockC &f, bool has_spec) {
        const State &st = d.st;
        for (int i = 0; i < 4; i++) memcpy(f.world[i], st.transforms[256 + i].m, 64);
        memcpy(f.view, st.transforms[2].m, 64);
        memcpy(f.proj, st.transforms[3].m, 64);
        for (int i = 0; i < 4; i++) {
            f.mat_diffuse[i] = f32(st.material[i]);
            f.mat_ambient[i] = f32(st.material[4 + i]);
            f.mat_specular[i] = f32(st.material[8 + i]);
            f.mat_emissive[i] = f32(st.material[12 + i]);
        }
        f.misc[0] = f32(st.material[16]);
        color4(st.rs[139], f.global_ambient);
        f.flags[0] = st.rs[137] != 0;
        f.flags[1] = st.rs[141] != 0;
        uint32_t vb = st.rs[151];
        f.flags[2] = vb >= 1 && vb <= 3 ? vb : 0;
        f.flags[3] = st.rs[143] != 0;
        f.sources[0] = st.rs[145];
        f.sources[1] = st.rs[146];
        f.sources[2] = st.rs[147];
        f.sources[3] = st.rs[148];
        f.fogv[0] = st.rs[28] && !st.rs[35] ? st.rs[140] : 0;
        f.fogv[1] = has_spec;
        uint32_t n = 0;
        for (auto &[l, on] : st.lights) {
            if (!on || n >= 8) continue;
            LightC &o = f.lights[n++];
            for (int i = 0; i < 4; i++) {
                o.diffuse[i] = f32(l.raw[1 + i]);
                o.specular[i] = f32(l.raw[5 + i]);
                o.ambient[i] = f32(l.raw[9 + i]);
            }
            for (int i = 0; i < 3; i++) {
                o.position[i] = f32(l.raw[13 + i]);
                o.direction[i] = f32(l.raw[16 + i]);
            }
            o.position[3] = float(l.raw[0]);
            o.att[0] = f32(l.raw[19]);
            o.att[1] = f32(l.raw[20]);
            o.att[2] = f32(l.raw[21]);
            o.att[3] = f32(l.raw[22]);
            o.att2[0] = f32(l.raw[23]);
            o.att2[1] = f32(l.raw[24]);
            o.att2[2] = f32(l.raw[25]);
            o.att2[3] = 1;
        }
        f.counts[0] = n;
    }

    CAMetalLayer *layer_;
    id<MTLDevice> dev_;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> lib_;
    dispatch_semaphore_t sem_;
    id<MTLBuffer> ring_[kFrames];
    size_t ring_off_ = 0;
    uint64_t frame_ = 0;
    id<MTLCommandBuffer> cmd_ = nil;
    id<MTLRenderCommandEncoder> enc_ = nil;
    id<MTLTexture> pass_color_ = nil, pass_depth_ = nil;
    Clear pending_;
    bool have_pending_ = false;
    bool state_dirty_ = false;
    id<MTLBuffer> defaults_;
    id<MTLDepthStencilState> clear_depth_, no_depth_;
    id<MTLRenderPipelineState> blit_ = nil;
    id<MTLSamplerState> linear_ = nil;
    id<MTLTexture> white_ = nil, movie_tex_ = nil;
    movie::Frame movie_;
    uint64_t movie_serial_ = 0;
    std::unordered_map<uint32_t, Tex> textures_;
    std::unordered_map<uint32_t, id<MTLTexture>> depth_;
    std::unordered_map<void *, bool> depth_fresh_;
    std::unordered_map<std::string, VBuf> vbufs_;
    std::unordered_map<uint32_t, IBuf> ibufs_;
    std::unordered_map<uint64_t, shader::Program> programs_;
    std::unordered_map<uint64_t, id<MTLFunction>> functions_;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> pipelines_;
    std::unordered_map<uint32_t, id<MTLDepthStencilState>> depth_states_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
};

}  // namespace

std::unique_ptr<Renderer> make_metal_renderer(void *layer) {
    return std::make_unique<MetalRenderer>((__bridge CAMetalLayer *)layer);
}

}  // namespace d3d
}  // namespace rt
