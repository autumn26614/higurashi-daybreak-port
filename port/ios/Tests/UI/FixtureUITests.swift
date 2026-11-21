import XCTest

/// Launches the app with the bundled fixture and checks what is actually on screen.
/// SwiftUI's List only creates the rows that are visible, so the checks go top to
/// bottom and scroll down for rows below the fold.
final class FixtureUITests: XCTestCase {
    override func setUp() {
        continueAfterFailure = false
    }

    /// Finds an element whose label contains `text`, whether SwiftUI exposes the value
    /// as its own static text or merges it into the row's label.
    private func element(containing text: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS %@", text)).firstMatch
    }

    /// Swipes up until an element containing `text` exists, or gives up after `swipes`.
    private func scrollTo(_ text: String, in app: XCUIApplication, swipes: Int = 8) -> Bool {
        let target = element(containing: text, in: app)
        for _ in 0..<swipes {
            if target.exists { return true }
            app.swipeUp()
        }
        return target.exists
    }

    /// No check marked ✗ among the rows currently on screen.
    private func assertNoFailedChecks(_ app: XCUIApplication, _ where_: String) {
        let failures = app.descendants(matching: .any).matching(NSPredicate(format: "identifier ENDSWITH '.fail'"))
        XCTAssertEqual(failures.count, 0, "a check is marked ✗ (\(where_))")
    }

    private func screenshot(_ app: XCUIApplication, _ name: String) {
        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = name
        shot.lifetime = .keepAlways
        add(shot)
    }

    func testFixtureIsReadAndJapaneseNameIsDisplayed() {
        let app = XCUIApplication()
        app.launchArguments = ["-useBundledFixture"]
        app.launch()

        let row = element(containing: "1 meshes / 1 tris", in: app)
        XCTAssertTrue(row.waitForExistence(timeout: 60), "mesh/triangle row never appeared on screen")
        XCTAssertFalse(app.staticTexts["error"].exists, "app reported an error")
        assertNoFailedChecks(app, "top of list")
        screenshot(app, "fixture-results")

        XCTAssertTrue(scrollTo("data/stage01/テスト草01.cnv", in: app), "archive entry name missing")
        XCTAssertTrue(element(containing: "テスト草01.bmp", in: app).exists, "Japanese texture name missing")
        assertNoFailedChecks(app, "after scrolling")
        screenshot(app, "fixture-results-scrolled")

        // Open the texture from the "All textures" list and check the decoded size.
        let textureRow = app.buttons["textureRow.0"]
        for _ in 0..<6 where !textureRow.exists { app.swipeUp() }
        XCTAssertTrue(textureRow.exists, "texture row not found")
        textureRow.tap()
        let size = app.staticTexts["textureSize"]
        XCTAssertTrue(size.waitForExistence(timeout: 30), "texture preview did not load")
        XCTAssertEqual(size.label, "2 × 2 px, 32 bpp")
        XCTAssertFalse(app.staticTexts["error"].exists, "texture preview reported an error")
        screenshot(app, "fixture-texture-preview")
    }

    /// Kai mode: overlay fixture searched before the base fixture.
    func testKaiOverlayIsSearchedFirst() {
        let app = XCUIApplication()
        app.launchArguments = ["-useBundledFixture", "-kai"]
        app.launch()

        let row = element(containing: "1 meshes / 2 tris", in: app)
        XCTAssertTrue(row.waitForExistence(timeout: 60), "overlay mesh (2 tris) never appeared on screen")
        XCTAssertTrue(element(containing: "fixture-overlay.dat → fixture.dat", in: app).exists, "search order missing")
        XCTAssertFalse(app.staticTexts["error"].exists, "app reported an error")
        assertNoFailedChecks(app, "top of list")
        screenshot(app, "fixture-kai-results")

        XCTAssertTrue(scrollTo("data/stage01/テスト草01.cnv", in: app), "archive entry name missing")
        XCTAssertTrue(element(containing: "テスト草01.bmp", in: app).exists, "Japanese texture name missing")
        assertNoFailedChecks(app, "after scrolling")
        screenshot(app, "fixture-kai-results-scrolled")
    }
}
