import XCTest
@testable import Daybreak

/// Characters, sounds, talk backgrounds and the music archives in the search order, on
/// the fixture (which has none of the first three) and test-made values.
final class MediaTests: XCTestCase {
    private func fixtureURL() throws -> URL {
        let url = Bundle.main.url(forResource: "fixture", withExtension: "dat")
            ?? Bundle(for: MediaTests.self).url(forResource: "fixture", withExtension: "dat")
        return try XCTUnwrap(url)
    }

    func testFixtureHasNoModelsOrSounds() throws {
        let url = try fixtureURL()
        XCTAssertEqual(CharacterModel.list(urls: [url]), [])
        XCTAssertThrowsError(try CharacterModel(urls: [url], entry: Array("data/rena/00.X".utf8)))
        let lib = try SoundLibrary(urls: [url])
        XCTAssertTrue(lib.names.isEmpty)
        XCTAssertNil(lib.voice("W05-08-1.wav"))
    }

    func testStoryScriptBackgroundIsBlack() throws {
        let book = try TalkBook(urls: [fixtureURL()])
        let bg = book.background(script: Array("data/scenario/D60-0.txt".utf8))
        XCTAssertEqual(bg.count, 800 * 600 * 4)
        XCTAssertTrue(stride(from: 0, to: bg.count, by: 4).allSatisfy { bg[$0] == 0 && bg[$0 + 3] == 255 })
        // Win scripts look up portraits; the fixture has none, so still black.
        let win = book.background(script: Array("data/scenario/win/05-08-4.txt".utf8))
        XCTAssertEqual(win.max(), 255)
        XCTAssertEqual(win[0], 0)
    }

    func testMusicArchivesFollowTheRequiredOnes() throws {
        func info(_ role: ArchiveRole) -> ArchiveInfo {
            ArchiveInfo(url: URL(fileURLWithPath: "/x/\(role.rawValue).dat"), role: role, entries: 1, scripts: 0,
                        englishScripts: 0)
        }
        XCTAssertEqual(try ArchiveOrder.order([info(.base01), info(.base00)], for: .base).map(\.role), [.base00, .base01])
        XCTAssertEqual(try ArchiveOrder.order([info(.kai05), info(.base00), info(.kai04), info(.base01)], for: .kai)
                        .map(\.role), [.kai04, .base00, .kai05], "Kai never opens daybreak01")
        XCTAssertThrowsError(try ArchiveOrder.order([info(.base01)], for: .base), "music alone is not enough")
    }

    func testDecodedSoundLoopsAndPlayerAcceptsIt() {
        let s = DecodedSound(channels: 2, rate: 22050, samples: [Int16](repeating: 100, count: 2000), loopStart: 100,
                             loopEnd: 900)
        XCTAssertEqual(s.frames, 1000)
        XCTAssertEqual(s.seconds, 1000.0 / 22050, accuracy: 1e-9)
        let player = SoundPlayer()
        player.play(s)  // no audio hardware needed for scheduling; must not crash
        player.stop()
    }
}
