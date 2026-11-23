import Foundation

/// One character model (port/src/character.h): skinned, with the game's animation sets.
/// Each pose comes back as stage-style batches with lighting baked into vertex colours, so
/// the Metal and CPU stage renderers draw it unchanged. The shading is an approximation:
/// the game uses compiled D3D9 shaders for characters that are not decoded.
final class CharacterModel {
    let name: String              // entry, e.g. "data/rena/00.X"
    let animations: [(name: String, seconds: Double)]
    let textures: [[UInt8]: StageTexture]
    private let archives: OpaquePointer
    private let character: OpaquePointer
    private let scopedURLs: [URL]

    /// Character model entries in the archives (data/<name>/0N.X), sorted.
    static func list(urls: [URL]) -> [[UInt8]] {
        withScopedAccess(urls) {
            var err = [CChar](repeating: 0, count: 256)
            guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
                return []
            }
            defer { db_archives_free(a) }
            return (0..<db_archives_character_count(a)).compactMap { i in
                var len: Int32 = 0
                guard let p = db_archives_character_name(a, i, &len) else { return nil }
                return Array(UnsafeRawBufferPointer(start: p, count: Int(len)))
            }
        }
    }

    /// Call off the main thread (a model is several MB of .x).
    init(urls: [URL], entry: [UInt8]) throws {
        let scoped = urls.filter { $0.startAccessingSecurityScopedResource() }
        var err = [CChar](repeating: 0, count: 256)
        guard let a = withCPaths(urls.map(\.path), { db_archives_open($0, $1, &err, Int32(err.count)) }) else {
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        let opened = entry.withUnsafeBufferPointer { b in
            b.baseAddress!.withMemoryRebound(to: CChar.self, capacity: b.count) {
                db_character_open(a, $0, Int32(b.count), &err, Int32(err.count))
            }
        }
        guard let c = opened else {
            db_archives_free(a)
            scoped.forEach { $0.stopAccessingSecurityScopedResource() }
            throw ReportError.open(message(err))
        }
        archives = a
        character = c
        scopedURLs = scoped
        name = String(data: Data(entry), encoding: StageReport.cp932) ?? "?"
        animations = (0..<db_character_anim_count(c)).map { i in
            var len: Int32 = 0
            let p = db_character_anim_name(c, i, &len)
            let n = p.map { String(decoding: UnsafeRawBufferPointer(start: $0, count: Int(len)), as: UTF8.self) } ?? ""
            return (n, db_character_anim_seconds(c, i))
        }
        // Textures: everything the bind pose references, decoded once.
        var textures: [[UInt8]: StageTexture] = [:]
        if let g = db_character_pose(c, -1, 0) {
            for b in StageScene.batches(of: g) where b.textureFound && textures[b.textureName] == nil {
                if let t = StageScene.loadTexture(a, b.textureName) { textures[b.textureName] = t }
            }
            db_geometry_free(g)
        }
        self.textures = textures
    }

    deinit {
        db_character_free(character)
        db_archives_free(archives)
        scopedURLs.forEach { $0.stopAccessingSecurityScopedResource() }
    }

    /// Batches of animation `anim` (-1: bind pose) at `seconds`, and a full-body camera.
    func pose(anim: Int, seconds: Double) -> (batches: [StageBatch], camera: StageCamera?) {
        guard let g = db_character_pose(character, Int32(anim), seconds) else { return ([], nil) }
        defer { db_geometry_free(g) }
        var cam = DBCamera()
        let camera = db_geometry_character_camera(g, &cam) == 1 ? StageCamera(cam) : nil
        return (StageScene.batches(of: g), camera)
    }

    /// The pose standing on a stage at `position`, turned to face `facing` (character slot).
    func pose(anim: Int, seconds: Double, at position: SIMD3<Float>, facing: SIMD3<Float>) -> [StageBatch] {
        var p = [position.x, position.y, position.z], f = [facing.x, facing.y, facing.z]
        let yaw = db_facing_yaw(&p, &f)
        guard let g = db_character_pose_at(character, Int32(anim), seconds, &p, yaw) else { return [] }
        defer { db_geometry_free(g) }
        return StageScene.batches(of: g)
    }

    /// CPU reference frame of a pose (for devices without Metal).
    func renderReference(anim: Int, seconds: Double, camera: StageCamera, width: Int, height: Int) -> [UInt8] {
        guard let g = db_character_pose(character, Int32(anim), seconds) else { return [] }
        defer { db_geometry_free(g) }
        var out = [UInt8](repeating: 0, count: width * height * 4)
        var cam = camera.db
        db_render_reference(archives, g, &cam, Int32(width), Int32(height), &out)
        return out
    }
}
