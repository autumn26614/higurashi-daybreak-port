import XCTest
@testable import Daybreak

/// The talk text box (port/src/talk.h) with real CoreText glyphs, on test-authored scripts.
final class TalkTests: XCTestCase {
    private func fixtureURL() throws -> URL {
        let url = Bundle.main.url(forResource: "fixture", withExtension: "dat")
            ?? Bundle(for: TalkTests.self).url(forResource: "fixture", withExtension: "dat")
        return try XCTUnwrap(url)
    }

    func testGlyphMasksFillTheGameCells() {
        let r = GlyphRasterizer()
        let a = r.mask(code: 0x41, bytes: 1)          // "A"
        XCTAssertEqual(a.width, 12)
        XCTAssertEqual(a.height, 24)
        XCTAssertGreaterThan(a.coverage.filter { $0 > 0 }.count, 20, "half-width glyph is drawn")
        XCTAssertLessThanOrEqual(a.coverage.max() ?? 0, 16, "GGO_GRAY4 levels")
        let hira = r.mask(code: 0x82A0, bytes: 2)     // "あ"
        XCTAssertEqual(hira.width, 24)
        XCTAssertGreaterThan(hira.coverage.filter { $0 > 0 }.count, 40, "full-width glyph is drawn")
        XCTAssertEqual(r.mask(code: 0x20, bytes: 1).coverage.max(), 0, "space is blank")
    }

    func testPagesPairVoiceAndText() {
        let script = Array("VOICE W01-01-1.wav\r\n\\cFF0000Hello\\nworld\r\nno voice\r\n".utf8)
        let pages = TalkBook.pages(ofScript: script)
        XCTAssertEqual(pages.count, 2)
        XCTAssertEqual(pages[0].voice, "W01-01-1.wav")
        XCTAssertEqual(pages[0].text, Array("\\cFF0000Hello\\nworld".utf8))
        XCTAssertEqual(pages[1].voice, "")
    }

    func testFrameRevealsText() throws {
        let book = try TalkBook(urls: [fixtureURL()])
        XCTAssertTrue(book.scriptNames.isEmpty, "the fixture has no scripts")
        XCTAssertFalse(book.hasWindowImage)
        let page = TalkPage(voice: "", text: Array("Hello\\nworld".utf8))
        func bright(_ rgba: [UInt8]) -> Int {
            var n = 0
            for y in 440..<600 { for x in 0..<800 where rgba[(y * 800 + x) * 4] > 200 { n += 1 } }
            return n
        }
        let hidden = book.frame(page, reveal: 0, arrow: false)
        XCTAssertEqual(hidden.end, 12, "strlen of the page")
        XCTAssertEqual(bright(hidden.rgba), 0, "nothing revealed yet")
        let shown = book.frame(page, reveal: hidden.end, arrow: false)
        XCTAssertGreaterThan(bright(shown.rgba), 100, "text drawn in the box")
        XCTAssertFalse(shown.stopped)
        // Only the box area is touched.
        XCTAssertEqual(shown.rgba[(100 * 800 + 400) * 4], 0)
        let image = try XCTUnwrap(TalkBook.cgImage(shown.rgba))
        XCTAssertEqual(image.width, 800)
        let shot = XCTAttachment(image: UIImage(cgImage: image))
        shot.name = "talk-frame"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
