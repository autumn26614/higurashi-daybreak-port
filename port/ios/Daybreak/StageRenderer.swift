import Metal
import MetalKit
import simd

/// Uniforms shared with Shaders.metal (StageUniforms): 176 bytes.
struct StageUniforms {
    var viewProj: simd_float4x4
    var view: simd_float4x4
    var fogColor: SIMD4<Float>
    var fogStart: Float
    var fogEnd: Float
    var fog: UInt32
    var alphaTest: UInt32
    var mode3: UInt32 = 0
    var pad0: UInt32 = 0, pad1: UInt32 = 0, pad2: UInt32 = 0
}

enum RendererError: LocalizedError {
    case noDevice, setup(String)
    var errorDescription: String? {
        switch self {
        case .noDevice: return "Metal is not available on this device"
        case .setup(let m): return "Metal setup failed: \(m)"
        }
    }
}

/// Draws a StageScene with Metal the way Stage::Draw (0x406440) does with D3D9: passes and
/// states from port/src/recipe.h, D3D matrices (Metal shares D3D's clip space), back-face
/// culling with clockwise front faces (D3D CULLMODE=CCW).
final class StageRenderer: NSObject, MTKViewDelegate {
    static let colorFormat = MTLPixelFormat.bgra8Unorm
    static let depthFormat = MTLPixelFormat.depth32Float

    let device: MTLDevice
    private let queue: MTLCommandQueue
    private var pipelines: [StagePass.Blend: MTLRenderPipelineState] = [:]
    private let depthWrite: MTLDepthStencilState
    private let depthNoWrite: MTLDepthStencilState
    private let linear: MTLSamplerState
    private let point: MTLSamplerState
    private var white: MTLTexture

    private struct GPUBatch {
        let slot: Int
        let power: Float
        let vertices: MTLBuffer
        let indices: MTLBuffer
        let indexCount: Int
        let texture: MTLTexture?
        let normals: MTLBuffer?  // characters: drawn again as an outline
    }
    private var outlinePipeline: MTLRenderPipelineState?
    /// Draw the characters' outline pass (game vertex shader 0x62FD94). Off for comparisons
    /// with the CPU reference renderer, which has no outline.
    var outlines = true
    private var gpu: [GPUBatch] = []
    private var dynamicGPU: [GPUBatch] = []  // e.g. animated characters on a stage, replaced per frame
    private(set) var scene: StageScene?

    var camera: StageCamera?
    /// Use the texture mip chain. Off for comparisons with the CPU reference renderer,
    /// which samples the base level only.
    var mipmaps = true

    init(device: MTLDevice? = MTLCreateSystemDefaultDevice(), colorFormat: MTLPixelFormat = colorFormat) throws {
        guard let device else { throw RendererError.noDevice }
        guard let queue = device.makeCommandQueue() else { throw RendererError.setup("no command queue") }
        self.device = device
        self.queue = queue

        let library: MTLLibrary
        do {
            library = try device.makeDefaultLibrary(bundle: Bundle(for: StageRenderer.self))
        } catch {
            throw RendererError.setup("shader library: \(error.localizedDescription)")
        }
        let vd = MTLVertexDescriptor()
        vd.attributes[0].format = .float3
        vd.attributes[0].offset = 0
        vd.attributes[1].format = .float2
        vd.attributes[1].offset = 12
        vd.attributes[2].format = .uchar4Normalized
        vd.attributes[2].offset = 20
        for i in 0..<3 { vd.attributes[i].bufferIndex = 0 }
        vd.layouts[0].stride = MemoryLayout<StageVertex>.stride

        for blend in [StagePass.Blend.alpha, .multiply, .additive] {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "stage_vertex")
            d.fragmentFunction = library.makeFunction(name: "stage_fragment")
            d.vertexDescriptor = vd
            d.depthAttachmentPixelFormat = Self.depthFormat
            let c = d.colorAttachments[0]!
            c.pixelFormat = colorFormat
            c.isBlendingEnabled = true
            switch blend {
            case .alpha:     // SRCALPHA, INVSRCALPHA
                c.sourceRGBBlendFactor = .sourceAlpha
                c.destinationRGBBlendFactor = .oneMinusSourceAlpha
            case .multiply:  // DESTCOLOR, ZERO
                c.sourceRGBBlendFactor = .destinationColor
                c.destinationRGBBlendFactor = .zero
            case .additive:  // SRCALPHA, ONE
                c.sourceRGBBlendFactor = .sourceAlpha
                c.destinationRGBBlendFactor = .one
            }
            // Destination alpha is never read; keep it opaque.
            c.sourceAlphaBlendFactor = .zero
            c.destinationAlphaBlendFactor = .one
            do {
                pipelines[blend] = try device.makeRenderPipelineState(descriptor: d)
            } catch {
                throw RendererError.setup("pipeline: \(error.localizedDescription)")
            }
        }

        // Outline: position from buffer 0, normal from buffer 2.
        let od = MTLVertexDescriptor()
        od.attributes[0].format = .float3
        od.attributes[0].offset = 0
        od.attributes[0].bufferIndex = 0
        od.attributes[3].format = .float3
        od.attributes[3].offset = 0
        od.attributes[3].bufferIndex = 2
        od.layouts[0].stride = MemoryLayout<StageVertex>.stride
        od.layouts[2].stride = 12
        let opd = MTLRenderPipelineDescriptor()
        opd.vertexFunction = library.makeFunction(name: "outline_vertex")
        opd.fragmentFunction = library.makeFunction(name: "outline_fragment")
        opd.vertexDescriptor = od
        opd.depthAttachmentPixelFormat = Self.depthFormat
        opd.colorAttachments[0].pixelFormat = colorFormat
        let outline = try? device.makeRenderPipelineState(descriptor: opd)

        func depth(_ write: Bool) -> MTLDepthStencilState? {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = .lessEqual  // D3DCMP_LESSEQUAL (default ZFUNC)
            d.isDepthWriteEnabled = write
            return device.makeDepthStencilState(descriptor: d)
        }
        func sampler(_ filter: MTLSamplerMinMagFilter, _ mip: MTLSamplerMipFilter) -> MTLSamplerState? {
            let d = MTLSamplerDescriptor()
            d.minFilter = filter
            d.magFilter = filter
            d.mipFilter = mip
            d.sAddressMode = .repeat  // D3DTADDRESS_WRAP
            d.tAddressMode = .repeat
            return device.makeSamplerState(descriptor: d)
        }
        guard let dw = depth(true), let dn = depth(false), let lin = sampler(.linear, .linear),
              let pt = sampler(.nearest, .nearest) else { throw RendererError.setup("states") }
        depthWrite = dw
        depthNoWrite = dn
        linear = lin
        point = pt

        // Placeholder until init finishes; replaced by a real white texture below.
        let td = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
        guard let placeholder = device.makeTexture(descriptor: td) else { throw RendererError.setup("texture") }
        white = placeholder
        outlinePipeline = outline
        super.init()
        // Untextured subsets (and textures missing from the archives) sample white, like
        // the CPU reference renderer; the result is modulated by the vertex colour.
        let w = StageTexture.Level(width: 1, height: 1, rgba: Data([255, 255, 255, 255]))
        if let tex = makeTexture(StageTexture(levels: [w])) { white = tex }
    }

    /// Fog and clear colour of what is drawn (a stage's env.dat; none for characters).
    struct Fog {
        var start: Float = 0, end: Float = 0
        var color = SIMD3<Float>(0, 0, 0)
    }
    var fog = Fog()
    /// Draw the stage passes marked mode3 with render mode 3 colours (a stage's plain set;
    /// off for the "b" set and for characters).
    var stageMode3 = false
    private var textureCache: [[UInt8]: MTLTexture] = [:]

    /// Uploads the scene's batches and textures. Textures get the game's authored mip
    /// levels where they exist; the rest of the chain is generated.
    func load(_ scene: StageScene) {
        setTextures(scene.textures)
        setBatches(scene.batches)
        fog = Fog(start: scene.fogStart, end: scene.fogEnd, color: scene.fogColor)
        stageMode3 = scene.mode3
        self.scene = scene
        if camera == nil { camera = scene.defaultCamera }
    }

    /// Replaces the texture set (uploaded once; batches refer to textures by entry name).
    func setTextures(_ textures: [[UInt8]: StageTexture]) {
        textureCache = [:]
        for (name, t) in textures {
            if let tex = makeTexture(t) { textureCache[name] = tex }
        }
    }

    /// Adds textures to the set already uploaded (characters joining a stage).
    func addTextures(_ textures: [[UInt8]: StageTexture]) {
        for (name, t) in textures where textureCache[name] == nil {
            if let tex = makeTexture(t) { textureCache[name] = tex }
        }
    }

    /// Replaces the geometry, keeping the textures: cheap enough to call every frame for
    /// an animated character (a few thousand vertices).
    func setBatches(_ batches: [StageBatch]) {
        gpu = upload(batches)
    }

    /// Replaces only the per-frame geometry drawn together with the batches above.
    func setDynamicBatches(_ batches: [StageBatch]) {
        dynamicGPU = upload(batches)
    }

    private func upload(_ batches: [StageBatch]) -> [GPUBatch] {
        batches.compactMap { b in
            guard !b.indices.isEmpty,
                  let vb = device.makeBuffer(bytes: b.vertices, length: b.vertices.count * MemoryLayout<StageVertex>.stride),
                  let ib = device.makeBuffer(bytes: b.indices, length: b.indices.count * 4)
            else { return nil }
            let nb = b.normals.count == b.vertices.count * 3 && !b.normals.isEmpty
                ? device.makeBuffer(bytes: b.normals, length: b.normals.count * 4) : nil
            return GPUBatch(slot: b.slot, power: b.power, vertices: vb, indices: ib, indexCount: b.indices.count,
                            texture: b.textureFound ? textureCache[b.textureName] : nil, normals: nb)
        }
    }

    /// Private-storage texture filled by blits from a staging buffer, which works the same
    /// on devices and in the Simulator.
    private func makeTexture(_ t: StageTexture) -> MTLTexture? {
        guard let base = t.levels.first else { return nil }
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: base.width,
                                                         height: base.height, mipmapped: true)
        d.usage = [.shaderRead, .renderTarget]
        d.storageMode = .private
        guard let tex = device.makeTexture(descriptor: d), let cb = queue.makeCommandBuffer(),
              let blit = cb.makeBlitCommandEncoder() else { return nil }
        func upload(_ level: StageTexture.Level, _ index: Int) {
            let bytes = level.width * level.height * 4
            guard level.rgba.count == bytes, bytes > 0,
                  let staging = level.rgba.withUnsafeBytes({ device.makeBuffer(bytes: $0.baseAddress!, length: bytes) })
            else { return }
            blit.copy(from: staging, sourceOffset: 0, sourceBytesPerRow: level.width * 4, sourceBytesPerImage: bytes,
                      sourceSize: MTLSize(width: level.width, height: level.height, depth: 1), to: tex,
                      destinationSlice: 0, destinationLevel: index, destinationOrigin: MTLOrigin(x: 0, y: 0, z: 0))
        }
        upload(base, 0)
        if tex.mipmapLevelCount > 1 { blit.generateMipmaps(for: tex) }
        for (i, level) in t.levels.enumerated().dropFirst() where i < tex.mipmapLevelCount {
            upload(level, i)  // the game's own mip images replace generated ones
        }
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        return tex
    }

    /// Encodes the stage into `encoder` (its pass must have colour + depth attachments).
    func encode(_ encoder: MTLRenderCommandEncoder, aspect: Float) {
        let all = gpu + dynamicGPU
        guard let camera, !all.isEmpty else { return }
        let m = camera.matrices(aspect: aspect)
        var u = StageUniforms(viewProj: m.proj * m.view, view: m.view, fogColor: SIMD4(fog.color, 1),
                              fogStart: fog.start, fogEnd: fog.end, fog: 0, alphaTest: 0)
        encoder.setCullMode(.back)
        encoder.setFrontFacing(.clockwise)  // D3D CULLMODE=CCW culls counter-clockwise faces
        for pass in StagePass.recipe {
            for b in all where b.slot == pass.slot {
                let p = pass.forBatch(power: b.power)
                guard let pipeline = pipelines[p.blend] else { continue }
                encoder.setRenderPipelineState(pipeline)
                encoder.setDepthStencilState(p.zWrite ? depthWrite : depthNoWrite)
                u.fog = p.fog ? 1 : 0
                u.alphaTest = p.alphaTest ? 1 : 0
                u.mode3 = stageMode3 && p.mode3 ? 1 : 0
                encoder.setVertexBytes(&u, length: MemoryLayout<StageUniforms>.stride, index: 1)
                encoder.setFragmentBytes(&u, length: MemoryLayout<StageUniforms>.stride, index: 1)
                encoder.setVertexBuffer(b.vertices, offset: 0, index: 0)
                encoder.setFragmentTexture(b.texture ?? white, index: 0)
                if mipmaps {
                    encoder.setFragmentSamplerState(p.pointSampling ? point : linear, index: 0)
                } else {
                    encoder.setFragmentSamplerState(p.pointSampling ? point : linear, lodMinClamp: 0, lodMaxClamp: 0,
                                                    index: 0)
                }
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: b.indexCount, indexType: .uint32,
                                              indexBuffer: b.indices, indexBufferOffset: 0)
            }
        }
        // Character outlines: back faces pushed outwards, black.
        if outlines, let outlinePipeline {
            encoder.setRenderPipelineState(outlinePipeline)
            encoder.setDepthStencilState(depthWrite)
            encoder.setCullMode(.front)
            encoder.setVertexBytes(&u, length: MemoryLayout<StageUniforms>.stride, index: 1)
            for b in all {
                guard let nb = b.normals else { continue }
                encoder.setVertexBuffer(b.vertices, offset: 0, index: 0)
                encoder.setVertexBuffer(nb, offset: 0, index: 2)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: b.indexCount, indexType: .uint32,
                                              indexBuffer: b.indices, indexBufferOffset: 0)
            }
            encoder.setCullMode(.back)
        }
    }

    private var clearColor: MTLClearColor {
        let c = fog.color
        return MTLClearColor(red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: 1)
    }

    // MARK: MTKViewDelegate

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let rp = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let cb = queue.makeCommandBuffer() else { return }
        rp.colorAttachments[0].clearColor = clearColor
        rp.colorAttachments[0].loadAction = .clear
        rp.depthAttachment.clearDepth = 1
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rp) else { return }
        let size = view.drawableSize
        encode(enc, aspect: Float(size.width / max(size.height, 1)))
        enc.endEncoding()
        cb.present(drawable)
        cb.commit()
    }

    /// Renders into an offscreen texture and returns RGBA bytes, top row first. For tests
    /// and snapshots. The renderer must have been created with colorFormat .rgba8Unorm.
    func renderOffscreen(width: Int, height: Int) throws -> [UInt8] {
        let cd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height,
                                                          mipmapped: false)
        cd.usage = [.renderTarget, .shaderRead]
        cd.storageMode = .private
        let dd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: Self.depthFormat, width: width, height: height,
                                                          mipmapped: false)
        dd.usage = .renderTarget
        dd.storageMode = .private
        guard let color = device.makeTexture(descriptor: cd), let depth = device.makeTexture(descriptor: dd),
              let readback = device.makeBuffer(length: width * height * 4, options: .storageModeShared),
              let cb = queue.makeCommandBuffer() else { throw RendererError.setup("offscreen targets") }
        let rp = MTLRenderPassDescriptor()
        rp.colorAttachments[0].texture = color
        rp.colorAttachments[0].loadAction = .clear
        rp.colorAttachments[0].storeAction = .store
        rp.colorAttachments[0].clearColor = clearColor
        rp.depthAttachment.texture = depth
        rp.depthAttachment.loadAction = .clear
        rp.depthAttachment.clearDepth = 1
        rp.depthAttachment.storeAction = .dontCare
        guard let enc = cb.makeRenderCommandEncoder(descriptor: rp) else { throw RendererError.setup("encoder") }
        enc.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(width), height: Double(height), znear: 0,
                                    zfar: 1))
        encode(enc, aspect: Float(width) / Float(height))
        enc.endEncoding()
        guard let blit = cb.makeBlitCommandEncoder() else { throw RendererError.setup("blit") }
        blit.copy(from: color, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                  sourceSize: MTLSize(width: width, height: height, depth: 1), to: readback, destinationOffset: 0,
                  destinationBytesPerRow: width * 4, destinationBytesPerImage: width * height * 4)
        blit.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        if let error = cb.error { throw RendererError.setup(error.localizedDescription) }
        var out = [UInt8](repeating: 0, count: width * height * 4)
        memcpy(&out, readback.contents(), out.count)
        for i in stride(from: 3, to: out.count, by: 4) { out[i] = 255 }
        return out
    }
}
