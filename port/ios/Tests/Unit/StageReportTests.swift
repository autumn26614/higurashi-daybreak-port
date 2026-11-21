import XCTest
@testable import DaybreakStageViewer

/// Swift → C API → C++ reader, on the synthetic fixture (port/tools/make_fixture.py).
final class StageReportTests: XCTestCase {
    private func fixturePath() throws -> String {
        // Hosted test: the fixture ships in the app bundle (Bundle.main); the test bundle
        // also carries a copy as a fallback.
        let url = Bundle.main.url(forResource: "fixture", withExtension: "dat")
            ?? Bundle(for: StageReportTests.self).url(forResource: "fixture", withExtension: "dat")
        return try XCTUnwrap(url, "fixture.dat not found in app or test bundle").path
    }

    func testFixtureCounts() throws {
        let r = try StageReport.load(path: fixturePath(), stage: 1)
        let e = ExpectedValues.fixture
        XCTAssertEqual(r.plainMeshes, e.plainMeshes)
        XCTAssertEqual(r.plainTriangles, e.plainTriangles)
        XCTAssertEqual(r.bMeshes, e.bMeshes)
        XCTAssertEqual(r.bTriangles, e.bTriangles)
        XCTAssertEqual(r.parseErrors, 0)
        XCTAssertEqual(r.textures.count, e.textures)
        XCTAssertEqual(r.resolvedCount, e.resolved)
    }

    func testShiftJISNameDecodes() throws {
        let r = try StageReport.load(path: fixturePath(), stage: 1)
        let jp = try XCTUnwrap(r.firstJapanese, "no texture name with non-ASCII bytes")
        XCTAssertEqual(jp.name, ExpectedValues.fixture.firstJapaneseName)
        // テ ス ト 草 0 1 . b m p, compared as code points so a lookalike cannot pass.
        XCTAssertEqual(jp.name.unicodeScalars.map(\.value),
                       [0x30C6, 0x30B9, 0x30C8, 0x8349, 0x30, 0x31, 0x2E, 0x62, 0x6D, 0x70])
        XCTAssertEqual(jp.archiveName, ExpectedValues.fixture.firstJapaneseArchiveName)
        XCTAssertTrue(jp.found)
    }

    func testMissingArchiveThrows() {
        XCTAssertThrowsError(try StageReport.load(path: "/nonexistent/daybreak00.dat", stage: 1))
    }

    private func overlayPath() throws -> String {
        let url = Bundle.main.url(forResource: "fixture-overlay", withExtension: "dat")
            ?? Bundle(for: StageReportTests.self).url(forResource: "fixture-overlay", withExtension: "dat")
        return try XCTUnwrap(url, "fixture-overlay.dat not found in app or test bundle").path
    }

    /// Kai-style search: the overlay's mesh wins, the texture falls through to the base.
    func testOverlaySearchedFirst() throws {
        let r = try StageReport.load(paths: [overlayPath(), fixturePath()], stage: 1)
        let e = ExpectedValues.fixtureOverlay
        XCTAssertEqual(r.plainMeshes, e.plainMeshes)
        XCTAssertEqual(r.plainTriangles, e.plainTriangles)
        XCTAssertEqual(r.parseErrors, 0)
        XCTAssertEqual(r.textures.count, e.textures)
        XCTAssertEqual(r.resolvedCount, e.resolved)
        XCTAssertEqual(r.firstJapanese?.name, e.firstJapaneseName)
    }

    /// Order is the whole rule: base first hides the overlay's mesh.
    func testSearchOrderMatters() throws {
        let r = try StageReport.load(paths: [fixturePath(), overlayPath()], stage: 1)
        XCTAssertEqual(r.plainTriangles, ExpectedValues.fixture.plainTriangles)
    }

    func testOverlayAloneHasNoTexture() throws {
        let r = try StageReport.load(paths: [overlayPath()], stage: 1)
        XCTAssertEqual(r.plainTriangles, 2)
        XCTAssertEqual(r.resolvedCount, 0, "the texture lives only in the base fixture")
    }

    func testSetWithMissingArchiveThrows() throws {
        XCTAssertThrowsError(try StageReport.load(paths: [fixturePath(), "/nonexistent/daybreak04.dat"], stage: 1))
        XCTAssertThrowsError(try StageReport.load(paths: [], stage: 1))
    }

    func testKaiOrderFromPickedFiles() throws {
        let base = URL(fileURLWithPath: "/x/DayBreak00.DAT")
        let kai = URL(fileURLWithPath: "/y/daybreak04.dat")
        XCTAssertEqual(try ArchiveOrder.kai([base, kai]), [kai, base])
        XCTAssertEqual(try ArchiveOrder.kai([kai, base]), [kai, base])
        XCTAssertThrowsError(try ArchiveOrder.kai([base]))
        XCTAssertThrowsError(try ArchiveOrder.kai([base, URL(fileURLWithPath: "/z/daybreak05.dat")]))
        XCTAssertThrowsError(try ArchiveOrder.kai([base, kai, URL(fileURLWithPath: "/z/daybreak04.dat")]))
    }
}
