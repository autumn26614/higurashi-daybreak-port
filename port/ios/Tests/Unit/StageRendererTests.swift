import Metal
import XCTest
import simd
@testable import DaybreakStageViewer

/// Stage geometry through the C API, the camera, and the Metal renderer checked pixel by
/// pixel against the CPU reference renderer (port/src/raster.cpp), on the fixtures.
final class StageRendererTests: XCTestCase {
    private func fixture(_ name: String) throws -> URL {
        let url = Bundle.main.url(forResource: name, withExtension: "dat")
            ?? Bundle(for: StageRendererTests.self).url(forResource: name, withExtension: "dat")
        return try XCTUnwrap(url, "\(name).dat not found")
    }

    /// The fixture triangle (0,0,0) (100,0,0) (0,100,0) faces +z: seen from +z it is
    /// clockwise on screen, i.e. a front face under D3D's CULLMODE=CCW.
    private let frontView = StageCamera(eye: SIMD3(33, 33, 250), target: SIMD3(33, 33, 0))
    private let backView = StageCamera(eye: SIMD3(33, 33, -250), target: SIMD3(33, 33, 0))

    func testFixtureSceneLoads() throws {
        let s = try StageScene(urls: [fixture("fixture")], stage: 1)
        XCTAssertEqual(s.batches.count, 1)
        XCTAssertEqual(s.triangleCount, 1)
        XCTAssertEqual(s.parseErrors, 0)
        let b = try XCTUnwrap(s.batches.first)
        XCTAssertEqual(b.slot, 0)
        XCTAssertEqual(b.vertices.count, 3)
        XCTAssertEqual(b.indices, [0, 1, 2])
        XCTAssertEqual(b.vertices[1].x, 100)
        XCTAssertTrue(b.textureFound)
        let t = try XCTUnwrap(s.textures[b.textureName])
        XCTAssertEqual(t.levels.first?.width, 2)
        XCTAssertEqual(t.levels.first?.height, 2)
        XCTAssertEqual(StageScene.availableStages(urls: [fixture("fixture")]), [1])
        XCTAssertThrowsError(try StageScene(urls: [fixture("fixture")], stage: 2), "no stage 02 in the fixture")
    }

    func testKaiOverlaySceneUsesOverlayMesh() throws {
        let s = try StageScene(urls: [fixture("fixture-overlay"), fixture("fixture")], stage: 1)
        XCTAssertEqual(s.triangleCount, 2, "the overlay's two-triangle mesh wins")
        XCTAssertEqual(s.textures.count, 1, "the texture still resolves from the base fixture")
    }

    func testRecipeIsTheGameOrder() {
        XCTAssertEqual(StagePass.recipe.map(\.slot), [1, 5, 4, 3, 6, 2, 0, 7])
        XCTAssertEqual(StagePass.recipe[2].blend, .multiply)
        XCTAssertFalse(StagePass.recipe[2].zWrite)
        let additive = StagePass.recipe[0].forBatch(power: 8)
        XCTAssertEqual(additive.blend, .additive)
        XCTAssertFalse(additive.zWrite)
        XCTAssertFalse(additive.fog)
        XCTAssertEqual(StagePass.recipe[0].forBatch(power: 0), StagePass.recipe[0])
    }

    func testCameraMatricesProjectLikeD3D() {
        let m = frontView.matrices(aspect: 4.0 / 3.0)
        func project(_ p: SIMD3<Float>) -> SIMD3<Float> {
            let c = m.proj * m.view * SIMD4(p, 1)
            return SIMD3(c.x, c.y, c.z) / c.w
        }
        let centre = project(frontView.target)
        XCTAssertEqual(centre.x, 0, accuracy: 1e-5)
        XCTAssertEqual(centre.y, 0, accuracy: 1e-5)
        XCTAssertGreaterThan(centre.z, 0)
        XCTAssertLessThan(centre.z, 1)
        // Looking along -z from +z: world +x appears on the left, +y up (left-handed).
        XCTAssertLessThan(project(SIMD3(100, 33, 0)).x, 0)
        XCTAssertGreaterThan(project(SIMD3(33, 100, 0)).y, 0)
        // View-space depth is the distance along the view direction.
        XCTAssertEqual((m.view * SIMD4(frontView.target, 1)).z, 250, accuracy: 1e-3)
    }

    func testOrbitCameraRoundTrip() {
        let cam = StageCamera(eye: SIMD3(318, 2674, -6504), target: SIMD3(318, 350, -1006))
        let o = OrbitCamera(cam)
        XCTAssertEqual(simd_distance(o.eye, cam.eye), 0, accuracy: 0.5)
        var z = o
        z.zoom(2)
        XCTAssertEqual(z.distance, o.distance / 2, accuracy: 0.01)
    }

    func testReferenceRendererCullsBackFaces() throws {
        let s = try StageScene(urls: [fixture("fixture")], stage: 1)
        XCTAssertGreaterThan(s.renderReference(camera: frontView, width: 64, height: 48).pixelsDrawn, 0)
        XCTAssertEqual(s.renderReference(camera: backView, width: 64, height: 48).pixelsDrawn, 0,
                       "counter-clockwise on screen: culled, as D3D CULLMODE=CCW")
    }

    /// The Metal output must match the CPU reference, pixel for pixel within rounding and
    /// a thin band of edge pixels (the two rasterisers place edges differently).
    private func compareMetalWithReference(_ urls: [URL], _ camera: StageCamera) throws {
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("no Metal device") }
        let scene = try StageScene(urls: urls, stage: 1)
        let renderer = try StageRenderer(colorFormat: .rgba8Unorm)
        renderer.mipmaps = false  // the reference samples the base level only
        renderer.load(scene)
        renderer.camera = camera
        let (w, h) = (64, 48)
        let metal = try renderer.renderOffscreen(width: w, height: h)
        let ref = scene.renderReference(camera: camera, width: w, height: h)
        XCTAssertGreaterThan(ref.pixelsDrawn, 100)
        var differing = 0, covered = 0
        for i in 0..<(w * h) {
            let d = (0..<3).map { abs(Int(metal[i * 4 + $0]) - Int(ref.rgba[i * 4 + $0])) }.max()!
            if d > 12 { differing += 1 }
            if ref.rgba[i * 4] > 0 { covered += 1 }
        }
        XCTAssertGreaterThan(covered, 100)
        XCTAssertLessThan(differing, covered / 10, "\(differing) of \(w * h) pixels differ from the reference")
        let shot = XCTAttachment(data: Data(metal), uniformTypeIdentifier: "public.data")
        shot.name = "metal-\(w)x\(h)-rgba"
        add(shot)
    }

    func testMetalMatchesReferenceOnFixture() throws {
        try compareMetalWithReference([fixture("fixture")], frontView)
    }

    func testMetalMatchesReferenceOnKaiOverlay() throws {
        try compareMetalWithReference([fixture("fixture-overlay"), fixture("fixture")],
                                      StageCamera(eye: SIMD3(50, 50, 250), target: SIMD3(50, 50, 0)))
    }

    func testMetalCullsBackFaces() throws {
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("no Metal device") }
        let scene = try StageScene(urls: [fixture("fixture")], stage: 1)
        let renderer = try StageRenderer(colorFormat: .rgba8Unorm)
        renderer.load(scene)
        renderer.camera = backView
        let px = try renderer.renderOffscreen(width: 32, height: 24)
        XCTAssertTrue(stride(from: 0, to: px.count, by: 4).allSatisfy { px[$0] == 0 && px[$0 + 1] == 0 },
                      "back face drawn")
    }
}
