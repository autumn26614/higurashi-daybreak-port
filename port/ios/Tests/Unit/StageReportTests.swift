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
}
