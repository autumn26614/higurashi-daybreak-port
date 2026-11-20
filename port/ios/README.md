# DaybreakStageViewer: iOS test app for the stage reader

A minimal SwiftUI app. You pick `daybreak00.dat` from the Files app, and it shows stage 01's mesh and triangle counts, texture-resolution results, and one Japanese filename decoded from cp932. Each value gets a ✓/✗ against the expected values below. It uses the same portable C++ reader as `port/stageinfo`, through the C API in `port/src/daybreak_c.h`. The archive is read in place and never modified.

```
port/
  src/            archive, inflate, .x parser, stage.cpp (shared), daybreak_c.{h,cpp} (C API)
  tests/capi_test.c   same C API calls as the app, runnable on Linux
  ios/
    project.yml                       XcodeGen spec
    DaybreakStageViewer/
      DaybreakStageViewerApp.swift    @main
      ContentView.swift               file picker + results
      StageReport.swift               Swift wrapper over the C API, cp932 decoding, expected values
      Bridging-Header.h               #include "daybreak_c.h"
```

## Status
- **Not yet built or run on iOS.** No Mac, Xcode or Swift toolchain was available; the Swift files have not been compiled.
- Verified on Linux (2026-11-20): the C++ part and the C API the app calls, via `tests/capi_test.c`, which makes the same calls in the same order. It reports the expected values below on x86-64 (WSL, g++ 15.2) and on aarch64 (cross g++ 15.2, run under qemu-aarch64 10.2.1). The first Japanese name's raw bytes decode to `くさむら06.bmp` with Python's cp932 codec.

## Build on a Mac
Requirements: Xcode 15 or newer, an iOS 16+ device or simulator. `daybreak00.dat` (1.5 GB) must be reachable from the Files app: AirDrop it to "On My iPhone", or for the simulator drag it into the simulator's Files app.

### Option A: XcodeGen (quickest)
```sh
brew install xcodegen
cd port/ios
xcodegen generate                   # → DaybreakStageViewer.xcodeproj
open DaybreakStageViewer.xcodeproj  # set your Team under Signing & Capabilities, then Run
# or from the command line, simulator build:
xcodebuild -project DaybreakStageViewer.xcodeproj -scheme DaybreakStageViewer \
  -destination 'platform=iOS Simulator,name=iPhone 15' build
```

### Option B: by hand in Xcode
1. File ▸ New ▸ Project ▸ iOS **App**. Name it `DaybreakStageViewer`, Interface SwiftUI, Language Swift. Save it in `port/ios/` and replace the generated Swift files with the three in `port/ios/DaybreakStageViewer/`.
2. Add `port/src/archive.cpp`, `inflate.cpp`, `xfile.cpp`, `stage.cpp`, `daybreak_c.cpp` (and their `.h` files) to the target. **Do not add `stageinfo.cpp`**, because it has its own `main`.
3. Build Settings for the target:
   - *Objective-C Bridging Header* = `DaybreakStageViewer/Bridging-Header.h`
   - *Header Search Paths* = `$(SRCROOT)/../src`
   - *C++ Language Dialect* = C++17
   - *Preprocessor Macros* add `DB_NO_ICONV=1` (the app decodes cp932 in Swift; this avoids linking libiconv)
   - *iOS Deployment Target* = 16.0
4. Set your signing team and run.

## Using it
Tap **Choose daybreak00.dat** and pick the file. If it lives in iCloud Drive, make sure it is downloaded first. Reading stage 01 parses the archive index plus 13 `.x` files and takes a few seconds; the device then shows:

| row | expected value |
|---|---|
| Plain set (game default) | **27 meshes / 28372 tris** |
| b set | **26 meshes / 28148 tris** |
| Textures: Referenced | **85** |
| Textures: Resolved in archive | **85** |
| Japanese filename: Texture | **くさむら06.bmp** (4th texture in raw-byte order; cp932 bytes `82 ad 82 b3 82 de 82 e7 30 36 2e 62 6d 70`) |
| Japanese filename: Archive entry | **data/stage01/くさむら06.cnv** (exists, 1 mip sibling) |
| All textures | 85 rows, all "ok"; the first three are `kumo.bmp`, `kumo01.bmp`, `shadow01.bmp`, the last is `ﾌﾞﾛｯｸ04.bmp` (half-width katakana) |

Every row should show a green ✓. Things worth noting if one doesn't:
- **Counts differ:** compare with `build/stageinfo <dat> 1` on Linux; an iOS-specific difference would point at the C++ build settings (e.g. C++ dialect).
- **Japanese name shows hex or wrong glyphs:** the cp932 decode in `StageReport.swift` (`kCFStringEncodingDOSJapanese`) failed. That is the one piece that is Apple-only and untested.
- **"Could not read the archive":** usually the file is still an iCloud placeholder, or it was picked without access. The app calls `startAccessingSecurityScopedResource()`.

## CI: simulator test without game data
`.github/workflows/ios-simulator.yml` (manual trigger only) runs on a `macos-15` runner:
1. Regenerates `Fixtures/fixture.dat` with `port/tools/make_fixture.py` and requires it to match the committed copy byte for byte.
2. Builds `port/` natively with clang and runs `capi_test … fixture`.
3. `xcodegen generate`, then `xcodebuild test` on the newest available iPhone simulator (`ci/pick_simulator.py`), unsigned (`CODE_SIGNING_ALLOWED=NO`).
4. Uploads the `.xcresult` (including a screenshot of the results screen) and the build log as the `ios-simulator-results` artifact.

The fixture is synthetic (1,193 bytes): one text `.x` mesh with a single triangle, whose material references `テスト草01.bmp`, plus that `.cnv` texture, all stored with Shift-JIS names in the real archive format. Tests:
- `Tests/Unit/StageReportTests.swift`: Swift → C API → C++ counts (1 mesh / 1 tri, 1 texture resolved) and the cp932 → `String` decode, compared code point by code point.
- `Tests/UI/FixtureUITests.swift`: launches the app with `-useBundledFixture` and checks that `テスト草01.bmp`, `data/stage01/テスト草01.cnv` and `1 meshes / 1 tris` appear on screen with no ✗.

Verified locally: the fixture reads back with the Python reader (`analysis/tools/dbdat.py`), with `stageinfo`, and with `capi_test … fixture` (PASS on x86-64, and on aarch64 under QEMU). The Swift and Xcode parts run for the first time on the CI runner.

## Linux check of the same C API
```sh
cd port && make                       # builds build/stageinfo and build/capi_test
build/capi_test "<…>/daybreak00.dat" 1  # prints the values above and PASS (exit 0)
```
