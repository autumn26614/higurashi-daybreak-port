import Foundation
import simd

/// One vertex as the Metal renderer consumes it (24 bytes, matches StageVertexIn).
struct StageVertex {
    var x: Float, y: Float, z: Float
    var u: Float, v: Float
    var r: UInt8, g: UInt8, b: UInt8, a: UInt8
}

/// One draw batch: a material subset of one mesh in one slot (port/src/geometry.h).
struct StageBatch {
    let slot: Int
    let power: Float          // != 0: drawn additively (no Z-write, no fog)
    let vertices: [StageVertex]
    let indices: [UInt32]
    let textureName: [UInt8]  // raw cp932 archive entry, empty if the material has none
    let textureFound: Bool
    var normals: [Float] = []  // xyz per vertex: characters, for the outline pass
}

/// A decoded texture with its authored mip levels (the game's "<name>_N" files), RGBA.
struct StageTexture {
    struct Level {
        let width: Int, height: Int
        let rgba: Data
    }
    let levels: [Level]  // levels[0] is the base image
}

/// One draw pass of the game's stage recipe (port/src/recipe.h, Stage::Draw 0x406440).
struct StagePass: Equatable {
    enum Blend: Int32 { case alpha = 0, multiply = 1, additive = 2 }
    let slot: Int
    let blend: Blend
    let zWrite: Bool
    let fog: Bool
    let alphaTest: Bool
    let pointSampling: Bool
    var mode3 = false

    /// Game order, from the C API.
    static let recipe: [StagePass] = (0..<db_stage_pass_count()).compactMap { i in
        var p = DBPass()
        guard db_stage_pass(i, &p) == 1 else { return nil }
        return StagePass(slot: Int(p.slot), blend: Blend(rawValue: p.blend) ?? .alpha, zWrite: p.z_write != 0,
                         fog: p.fog != 0, alphaTest: p.alpha_test != 0, pointSampling: p.point_sampling != 0,
                         mode3: p.mode3 != 0)
    }

    /// A batch whose material power is non-zero is additive, no Z-write, no fog (0x5D1750).
    func forBatch(power: Float) -> StagePass {
        guard power != 0 else { return self }
        return StagePass(slot: slot, blend: .additive, zWrite: false, fog: false, alphaTest: alphaTest,
                         pointSampling: pointSampling, mode3: mode3)
    }
}

/// Camera in the game's convention (Direct3D, left-handed, y up), as DBCamera.
struct StageCamera: Equatable {
    var eye: SIMD3<Float>
    var target: SIMD3<Float>
    var fovY: Float = .pi / 4
    var zNear: Float = 50
    var zFar: Float = 200_000

    init(eye: SIMD3<Float>, target: SIMD3<Float>) {
        self.eye = eye
        self.target = target
    }

    init(_ c: DBCamera) {
        eye = SIMD3(c.eye.0, c.eye.1, c.eye.2)
        target = SIMD3(c.target.0, c.target.1, c.target.2)
        fovY = c.fov_y
        zNear = c.z_near
        zFar = c.z_far
    }

    var db: DBCamera {
        DBCamera(eye: (eye.x, eye.y, eye.z), target: (target.x, target.y, target.z), fov_y: fovY, z_near: zNear,
                 z_far: zFar)
    }

    /// View and projection as column-major simd matrices for column vectors. The C API
    /// returns D3D row-major matrices for row vectors; loading their rows as columns
    /// gives the transposes, which is exactly that (M^T * v == v * M).
    func matrices(aspect: Float) -> (view: simd_float4x4, proj: simd_float4x4) {
        var cam = db
        var v = [Float](repeating: 0, count: 16), p = [Float](repeating: 0, count: 16)
        db_camera_matrices(&cam, aspect, &v, &p)
        func load(_ m: [Float]) -> simd_float4x4 {
            simd_float4x4(SIMD4(m[0], m[1], m[2], m[3]), SIMD4(m[4], m[5], m[6], m[7]),
                          SIMD4(m[8], m[9], m[10], m[11]), SIMD4(m[12], m[13], m[14], m[15]))
        }
        return (load(v), load(p))
    }
}

/// Everything needed to draw one stage, copied out of the archives once. Keeps the
/// archives and the C geometry open for the CPU reference renderer.
final class StageScene {
    let stage: Int
    let variant: Int
    let batches: [StageBatch]
    let textures: [[UInt8]: StageTexture]  // by archive entry name
    let triangleCount: Int
    let fogStart: Float, fogEnd: Float
    let fogColor: SIMD3<Float>             // 0...1
    let defaultCamera: StageCamera
    let parseErrors: Int
    let mode3: Bool  // drawn in render mode 3 (plain set)
    let spawns: [SIMD3<Float>]  // env.dat players 1-4

    private let archives: OpaquePointer
    private let geometry: OpaquePointer
    private let scopedURLs: [URL]

    /// Opens `urls` (search order, as StageReport.load) and loads `stage`. Holds
    /// security-scoped access to the files for the scene's lifetime. Call off the main thread.
    init(urls: [URL], stage: Int, variant: Int = 0) throws {
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        var err = [CChar](repeating: 0, count: 256)
        let opened = withCPaths(urls.map(\.path)) { argv, count in db_archives_open(argv, count, &err, Int32(err.count)) }
        guard let archives = opened else {
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        guard let geometry = db_geometry_open(archives, Int32(stage), Int32(variant), &err, Int32(err.count)) else {
            db_archives_free(archives)
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        self.archives = archives
        scopedURLs = scoped
        self.geometry = geometry
        self.stage = stage
        self.variant = variant

        let batches = StageScene.batches(of: geometry)
        var textures: [[UInt8]: StageTexture] = [:]
        for b in batches where b.textureFound && textures[b.textureName] == nil {
            if let t = StageScene.loadTexture(archives, b.textureName) { textures[b.textureName] = t }
        }
        self.batches = batches
        self.textures = textures
        triangleCount = Int(db_geometry_triangle_count(geometry))
        parseErrors = Int(db_geometry_error_count(geometry))
        mode3 = db_geometry_mode3(geometry) == 1

        var env = DBStageEnv()
        db_geometry_env(geometry, &env)
        fogStart = Float(env.fog_start)
        fogEnd = Float(env.fog_end)
        fogColor = SIMD3(Float(env.fog_rgb.0), Float(env.fog_rgb.1), Float(env.fog_rgb.2)) / 255
        let sp = env.spawn
        spawns = [sp.0, sp.1, sp.2, sp.3].map { SIMD3($0.0, $0.1, $0.2) }
        var cam = DBCamera()
        db_geometry_default_camera(geometry, &cam)
        defaultCamera = StageCamera(cam)
    }

    deinit {
        db_geometry_free(geometry)
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    /// Stages present in the archives (any visual mesh file), 1...99.
    static func availableStages(urls: [URL]) -> [Int] {
        withScopedAccess(urls) {
            var err = [CChar](repeating: 0, count: 256)
            guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
                return []
            }
            defer { db_archives_free(a) }
            return (1...99).filter { db_archives_has_stage(a, Int32($0)) == 1 }
        }
    }

    /// The base image plus the game's authored mip files "<name>_1.cnv", "_2", … while each
    /// halves the previous size (the texture loader 0x5D8830 probes them the same way).
    /// Copies every batch of a DBStageGeometry into Swift arrays.
    static func batches(of geometry: OpaquePointer) -> [StageBatch] {
        var batches: [StageBatch] = []
        for i in 0..<db_geometry_batch_count(geometry) {
            var b = DBBatch()
            guard db_geometry_batch(geometry, i, &b) == 1 else { continue }
            let n = Int(b.vertex_count)
            var vertices = [StageVertex]()
            vertices.reserveCapacity(n)
            for v in 0..<n {
                vertices.append(StageVertex(
                    x: b.positions[v * 3], y: b.positions[v * 3 + 1], z: b.positions[v * 3 + 2],
                    u: b.uvs[v * 2], v: b.uvs[v * 2 + 1],
                    r: b.colors[v * 4], g: b.colors[v * 4 + 1], b: b.colors[v * 4 + 2], a: b.colors[v * 4 + 3]))
            }
            let indices = Array(UnsafeBufferPointer(start: b.indices, count: Int(b.index_count)))
            let name: [UInt8] = b.texture_len > 0
                ? Array(UnsafeRawBufferPointer(start: b.texture, count: Int(b.texture_len))) : []
            batches.append(StageBatch(slot: Int(b.slot), power: b.power, vertices: vertices, indices: indices,
                                      textureName: name, textureFound: b.texture_found != 0,
                                      normals: b.normals.map { Array(UnsafeBufferPointer(start: $0, count: n * 3)) } ?? []))
        }
        return batches
    }

    static func loadTexture(_ archives: OpaquePointer, _ name: [UInt8]) -> StageTexture? {
        guard let base = decode(archives, name) else { return nil }
        var levels = [base]
        let stem = name.count > 4 ? Array(name.dropLast(4)) : name  // drop ".cnv"
        for n in 1..<8 {
            let w = max(1, base.width >> n), h = max(1, base.height >> n)
            guard let level = decode(archives, stem + Array("_\(n).cnv".utf8)), level.width == w, level.height == h
            else { break }
            levels.append(level)
        }
        return StageTexture(levels: levels)
    }

    private static func decode(_ archives: OpaquePointer, _ name: [UInt8]) -> StageTexture.Level? {
        var err = [CChar](repeating: 0, count: 256)
        let image = name.withUnsafeBufferPointer { bytes in
            bytes.baseAddress!.withMemoryRebound(to: CChar.self, capacity: bytes.count) {
                db_image_load(archives, $0, Int32(bytes.count), &err, Int32(err.count))
            }
        }
        guard let image else { return nil }
        defer { db_image_free(image) }
        let w = Int(db_image_width(image)), h = Int(db_image_height(image))
        return StageTexture.Level(width: w, height: h, rgba: Data(bytes: db_image_rgba(image), count: w * h * 4))
    }

    /// CPU reference render (port/src/raster.cpp), RGBA, alpha 255. Call off the main thread.
    func renderReference(camera: StageCamera, width: Int, height: Int) -> (rgba: [UInt8], pixelsDrawn: Int) {
        var out = [UInt8](repeating: 0, count: width * height * 4)
        var cam = camera.db
        let drawn = db_render_reference(archives, geometry, &cam, Int32(width), Int32(height), &out)
        return (out, Int(drawn))
    }
}
