# DaybreakStageViewer: iOS test app for the stage reader

A minimal SwiftUI app. You pick `daybreak00.dat` from the Files app, and it shows stage 01's mesh and triangle counts, texture-resolution results, and one Japanese filename decoded from cp932. Each value gets a ✓/✗ against the expected values below. An "Archives" section shows what each file is and whether the English fan translation is applied (ASCII-only scenario scripts). It uses the same portable C++ reader as `port/stageinfo`, through the C API in `port/src/daybreak_c.h`. The archive is read in place and never modified.

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
- Built and tested in CI on an Intel macOS runner (iOS Simulator) against the synthetic fixtures; not yet run on a physical device or with the retail archives.
- Verified on Linux (2026-11-20): the C++ part and the C API the app calls, via `tests/capi_test.c`, which makes the same calls in the same order. It reports the expected values below on x86-64 (WSL, g++ 15.2) and on aarch64 (cross g++ 15.2, run under qemu-aarch64 10.2.1). The first Japanese name's raw bytes decode to `くさむら06.bmp` with Python's cp932 codec.

## Build on a Mac
Requirements: Xcode 15 or newer, an iOS 16+ device or simulator. Get `daybreak00.dat` (1.5 GB) onto the device in one of two ways:
- Copy it into the app's own folder: Files app → On My iPhone → Daybreak Stage, or Finder file sharing from a Mac. The app finds archives there at launch, whatever their names.
- Or keep it anywhere the Files app reaches (iCloud Drive, another app's folder) and pick it with the button. The app remembers the pick (a bookmark) and reopens it at the next launch.

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

## Kai expansion mode
Switch the segmented control to **Kai**, tap **Choose daybreak00.dat + daybreak04.dat**, and select both files in one go. `daybreak04.dat` comes from the Kai disc (`DATA/DAYBRE~1.DAT` in the disc's 8.3 directory, `daybreak04.dat` under its long-name Joliet names). The app identifies each file by its contents (entry count plus a known entry, so a renamed copy still works; `port/src/identify.h`) and searches **daybreak04 first, then daybreak00**, as DaybreakDX.exe does (`analysis/07-kai-archive-lookup.md`). The "Search order" row shows the order used. It reads stage 16, which exists only in daybreak04, so a correct result cannot come from the base archive alone:

| row | expected value |
|---|---|
| Plain set / b set | **40 meshes / 23961 tris** each |
| Textures: Referenced / Resolved | **38 / 38** |
| Japanese filename: Texture | **アスファルト05.bmp** (cp932 `83 41 83 58 83 74 83 40 83 8b 83 67 30 35 2e 62 6d 70`) |
| Japanese filename: Archive entry | **data/stage16/アスファルト05.cnv** (2 mip siblings) |

The same C call (`db_stage_report_open_set`, 04 then 00) was checked on Linux against the real archives with `build/capi_test <daybreak00.dat> 16 kai <daybreak04.dat>`. It passes on x86-64 and on aarch64 under QEMU, and fails when daybreak00 is passed in place of daybreak04. The Swift side runs first in CI.

## Texture preview
Every resolved row under **All textures** opens a preview. The preview decodes that `.cnv` from the same archives, in the same search order, through `db_archives_open` / `db_image_load`. It shows the texture at its stored pixels without smoothing, with its size and bpp. Decoding was checked against all 5,418 images in daybreak00 and Kai daybreak04 on Linux (`port/README.md`, "Textures"). On iOS it is so far exercised only with the fixture texture: 2×2 red, green, blue and white.

## Install on an iPhone without a Mac (Windows)
1. Run the **iOS device build (unsigned)** workflow (`gh workflow run ios-device.yml`) and download the `DaybreakStageViewer-unsigned-ipa` artifact. The workflow fails if the app contains any `.dat` other than the two synthetic fixtures, or any file over 20 MB, so no game data can end up in it.
2. Sign and install it from Windows with a sideloading tool such as Sideloadly and your Apple ID. A free Apple ID signs apps for 7 days, after which you re-sign. A paid developer account allows TestFlight instead; that needs signing secrets in CI and is not set up.
3. On the iPhone (iOS 16+): Settings → Privacy & Security → Developer Mode → on, then trust the developer profile under Settings → General → VPN & Device Management.
4. Copy `daybreak00.dat` (and `daybreak04.dat` for Kai) into Files → On My iPhone → Daybreak Stage, then open the app.

## CI: simulator test without game data
`.github/workflows/ios-simulator.yml` (manual trigger, or a push to main that changes the workflow or port/) runs on `macos-15-intel` (x86_64) and, experimentally, `macos-15` (arm64):
1. Regenerates `Fixtures/fixture.dat` and `fixture-overlay.dat` with `port/tools/make_fixture.py` and requires both to match the committed copies byte for byte.
2. Builds `port/` natively with clang and runs `capi_test … fixture` and `make test`.
3. `xcodegen generate`, then `xcodebuild test` on the newest available iPhone simulator (`ci/pick_simulator.py`), unsigned (`CODE_SIGNING_ALLOWED=NO`).
4. Uploads the `.xcresult` (including a screenshot of the results screen) and the build log as the `ios-simulator-results` artifact.

The fixture is synthetic (1,193 bytes): one text `.x` mesh with a single triangle, whose material references `テスト草01.bmp`, plus that `.cnv` texture, all stored with Shift-JIS names in the real archive format. Tests:
- `Tests/Unit/StageReportTests.swift`: Swift → C API → C++ counts (1 mesh / 1 tri, 1 texture resolved) and the cp932 → `String` decode, compared code point by code point.
- `Tests/UI/FixtureUITests.swift`: launches the app with `-useBundledFixture` and checks that `テスト草01.bmp`, `data/stage01/テスト草01.cnv` and `1 meshes / 1 tris` appear on screen with no ✗.

A second fixture, `Fixtures/fixture-overlay.dat` (575 bytes, written by the same script), holds only `DATA/Stage01/00.X`. That is the same mesh with two triangles, under a differently cased name. It stands in for daybreak04:
- Unit: `[overlay, base]` gives 1 mesh / 2 tris with the texture resolved from the base, and `[base, overlay]` gives 1 tri. The overlay alone resolves no texture. A missing archive or an empty list throws. `ArchiveOrder.kai` orders picked files 04 → 00 and rejects any other selection.
- UI: `-useBundledFixture -kai` must show `1 meshes / 2 tris`, the search order `fixture-overlay.dat → fixture.dat` and no ✗.

Texture preview: the unit tests decode the fixture texture to the exact RGBA bytes and check that the `CGImage` hands out those bytes unchanged (straight alpha). They also check the error cases: missing name, empty name, missing archive. The base UI test taps the texture row and expects `2 × 2 px, 32 bpp`.

Verified locally: the fixture reads back with the Python reader (`analysis/tools/dbdat.py`), with `stageinfo`, and with `capi_test … fixture` (PASS on x86-64, and on aarch64 under QEMU). The Swift and Xcode parts run for the first time on the CI runner.

## Linux check of the same C API
```sh
cd port && make                       # builds build/stageinfo and build/capi_test
build/capi_test "<…>/daybreak00.dat" 1  # prints the values above and PASS (exit 0)
```
