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

    /// The fixture texture through the report's own entry name: 2x2 red, green, blue, white.
    func testTextureDecodes() throws {
        let r = try StageReport.load(paths: [overlayPath(), fixturePath()], stage: 1)
        let t = try XCTUnwrap(r.textures.first)
        let image = try TextureImage.load(paths: [overlayPath(), fixturePath()], name: t.archiveNameBytes)
        XCTAssertEqual(image.width, 2)
        XCTAssertEqual(image.height, 2)
        XCTAssertEqual(image.bpp, 32)
        XCTAssertEqual(Array(image.rgba), [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255])
        XCTAssertEqual(image.cgImage.width, 2)
        XCTAssertEqual(image.cgImage.alphaInfo, .last)
        // The bytes the CGImage hands out are the decoded ones, unchanged.
        let data = try XCTUnwrap(image.cgImage.dataProvider?.data) as Data
        XCTAssertEqual(data, image.rgba)
    }

    func testTextureErrors() throws {
        XCTAssertThrowsError(try TextureImage.load(paths: [fixturePath()], name: Array("data/stage01/none.bmp".utf8)))
        XCTAssertThrowsError(try TextureImage.load(paths: [fixturePath()], name: []))
        XCTAssertThrowsError(try TextureImage.load(paths: ["/nonexistent/daybreak00.dat"], name: Array("a.bmp".utf8)))
    }

    private func info(_ path: String, _ role: ArchiveRole) -> ArchiveInfo {
        ArchiveInfo(url: URL(fileURLWithPath: path), role: role, entries: 1, scripts: 0, englishScripts: 0)
    }

    /// Order comes from what each file is, not from its name or the pick order.
    func testKaiOrderFromIdentifiedFiles() throws {
        let base = info("/x/DayBreak00.DAT", .base00)
        let kai = info("/y/renamed.dat", .kai04)
        XCTAssertEqual(try ArchiveOrder.order([base, kai], for: .kai), [kai, base])
        XCTAssertEqual(try ArchiveOrder.order([kai, base], for: .kai), [kai, base])
        XCTAssertEqual(try ArchiveOrder.order([base], for: .base), [base])
        XCTAssertEqual(try ArchiveOrder.order([info("/z/daybreak05.dat", .kai05), base], for: .base), [base],
                       "extra archives are ignored")
        XCTAssertThrowsError(try ArchiveOrder.order([base], for: .kai))
        XCTAssertThrowsError(try ArchiveOrder.order([kai], for: .base))
        XCTAssertThrowsError(try ArchiveOrder.order([], for: .base))
    }

    /// A wrong pick says what the file really is.
    func testOrderErrorNamesRenamedFiles() {
        let renamed = info("/x/daybreak00.dat", .kai04)
        XCTAssertThrowsError(try ArchiveOrder.order([renamed], for: .base)) { error in
            let text = error.localizedDescription
            XCTAssertTrue(text.contains("Needs daybreak00.dat"), text)
            XCTAssertTrue(text.contains("daybreak00.dat (is daybreak04.dat)"), text)
        }
        XCTAssertThrowsError(try ArchiveOrder.order([info("/x/other.dat", .unknown)], for: .base)) { error in
            XCTAssertTrue(error.localizedDescription.contains("not a Daybreak archive"), error.localizedDescription)
        }
    }

    func testIdentifyFixture() throws {
        let i = try ArchiveInfo.identify(URL(fileURLWithPath: fixturePath()))
        XCTAssertEqual(i.role, .unknown, "the fixture is not a retail archive")
        XCTAssertEqual(i.entries, 3)
        XCTAssertEqual(i.languageSummary, "no scripts")
        XCTAssertThrowsError(try ArchiveInfo.identify(URL(fileURLWithPath: "/nonexistent/daybreak00.dat")))
        XCTAssertEqual(ArchiveRole.kai04.fileName, "daybreak04.dat")
        XCTAssertEqual(ArchiveRole.unknown.fileName, "")
    }

    func testBookmarksRoundTrip() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let a = dir.appendingPathComponent("daybreak04.dat"), b = dir.appendingPathComponent("daybreak00.dat")
        try FileManager.default.copyItem(atPath: overlayPath(), toPath: a.path)
        try FileManager.default.copyItem(atPath: fixturePath(), toPath: b.path)

        let suite = "bookmarks-test-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = BookmarkStore(defaults: defaults)
        XCTAssertNil(store.load(for: .kai))
        try store.save([a, b], for: .kai)
        let loaded = try XCTUnwrap(store.load(for: .kai))
        XCTAssertEqual(loaded.map { $0.resolvingSymlinksInPath().path },
                       [a, b].map { $0.resolvingSymlinksInPath().path }, "order is kept")
        XCTAssertNil(store.load(for: .base), "modes are stored separately")

        // The saved set still opens as a Kai-style search.
        let r = try StageReport.load(paths: loaded.map(\.path), stage: 1)
        XCTAssertEqual(r.plainTriangles, ExpectedValues.fixtureOverlay.plainTriangles)

        try FileManager.default.removeItem(at: a)
        XCTAssertNil(store.load(for: .kai), "a deleted file invalidates the saved set")
        store.clear(for: .kai)
        XCTAssertNil(store.load(for: .kai))
    }

    func testDocumentsScan() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        try FileManager.default.copyItem(atPath: fixturePath(), toPath: dir.appendingPathComponent("b.DAT").path)
        try FileManager.default.copyItem(atPath: overlayPath(), toPath: dir.appendingPathComponent("a.dat").path)
        try Data("not an archive".utf8).write(to: dir.appendingPathComponent("c.dat"))
        try Data().write(to: dir.appendingPathComponent("notes.txt"))
        let found = DocumentsFolder.scan(dir)
        XCTAssertEqual(found.map { $0.url.lastPathComponent }, ["a.dat", "b.DAT"], "corrupt and non-.dat files skipped")
        XCTAssertEqual(found.map(\.entries), [1, 3])
    }
}
