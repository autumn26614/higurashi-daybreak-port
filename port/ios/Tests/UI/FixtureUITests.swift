import XCTest

/// Launches the app with the bundled fixture and checks what is actually on screen.
final class FixtureUITests: XCTestCase {
    override func setUp() {
        continueAfterFailure = false
    }

    /// Finds an element whose label contains `text`, whether SwiftUI exposes the value
    /// as its own static text or merges it into the row's label.
    private func element(containing text: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS %@", text)).firstMatch
    }

    func testFixtureIsReadAndJapaneseNameIsDisplayed() {
        let app = XCUIApplication()
        app.launchArguments = ["-useBundledFixture"]
        app.launch()

        let name = element(containing: "テスト草01.bmp", in: app)
        XCTAssertTrue(name.waitForExistence(timeout: 60), "Japanese texture name never appeared on screen")
        XCTAssertTrue(element(containing: "data/stage01/テスト草01.cnv", in: app).exists, "archive entry name missing")
        XCTAssertTrue(element(containing: "1 meshes / 1 tris", in: app).exists, "mesh/triangle row missing")
        XCTAssertFalse(app.staticTexts["error"].exists, "app reported an error")

        // If SwiftUI keeps the per-value identifiers, also require that no check is red.
        let failures = app.descendants(matching: .any).matching(NSPredicate(format: "identifier ENDSWITH '.fail'"))
        XCTAssertEqual(failures.count, 0, "a check on screen is marked ✗")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "fixture-results"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
