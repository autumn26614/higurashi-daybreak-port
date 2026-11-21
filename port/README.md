# port/: portable readers for Higurashi Daybreak data

The first piece of an eventual ARM64/iOS port is a read-only reader for the stage data. It is self-contained C++17: no zlib, no DirectX, and the only optional dependency is glibc `iconv` for Shift-JIS names. It builds unchanged for x86-64 Linux/WSL and for arm64 (it assumes a little-endian host, which covers both).

## Build (WSL / Linux)
```sh
sudo apt install -y g++ make     # once
cd port && make                  # → build/stageinfo
```

## Use
```sh
D="/path/to/Higurashi Daybreak (Japan)/data"
build/stageinfo "$D/daybreak00.dat" 1            # stage 01: env, mesh slots, collision, textures
build/stageinfo "$D/daybreak00.dat" 1 --tree     # + per-mesh lines
build/stageinfo "$D/daybreak00.dat" --x data/stage01/collision.x
build/stageinfo "$D/daybreak00.dat" --stages
```
The tool only reads the archive and never writes anything.

Kai expansion: add `--overlay daybreak04.dat` (any form above). Overlays are searched before `daybreak00.dat`, in the order given, and the first archive that has a name wins. This is the same as `File::Open` in DaybreakDX.exe (`0x6510a0`, see `analysis/07-kai-archive-lookup.md`).
```sh
build/stageinfo "$D/daybreak00.dat" --overlay "$KAI/daybreak04.dat" 16
```

## What it implements
| file | what |
|---|---|
| `src/archive.*` | `daybreakNN.dat`: index rolling-XOR, per-entry XOR, case-insensitive lookup (see `analysis/02-archive-format.md`); `ArchiveSet` = ordered multi-archive search (Kai: 04 then 00) |
| `src/inflate.*` | raw DEFLATE with preset dictionary (for MSZIP `.x`) |
| `src/xfile.*` | DirectX `.x` `txt ` / `bin ` / `bzip` (and `tzip`) → generic object tree |
| `src/stage.*` | shared analysis, following `Stage::Load` (0x405B50): slots `%02d.x`/`%02db.x`, mesh/triangle counts, and texture resolution exactly as the engine does it (dir + name, `bmp`→`cnv`, `_N` mips) |
| `src/stageinfo.cpp` | CLI front end (env.dat, slots, three collision meshes, texture list) |
| `src/daybreak_c.*` | plain C API over `stage.*` for Swift / other callers; names returned as raw cp932 bytes |
| `tests/capi_test.c` | C caller of the API; checks the stage 01 expected values (PASS/FAIL) |
| `tests/archive_set_test.cpp` | `make test`: writes two synthetic archives and checks lookup order, fallthrough, case-insensitivity, per-archive reads and `db_stage_report_open_set` |
| `ios/` | SwiftUI test app over the C API; see `ios/README.md` (not yet built: needs a Mac) |

## Sample output (stage 01; abridged)
```
Stage 01 (data/stage01)
env.dat: fog? start 0 end 60000 colour #4D6478 | params -2264 -3406 2900 1393
         spawn 1p (-463, 200, -2200) ...
Visual mesh slots 00-07 (game loads "%02d.x" when Stage+0x2C != 0, else "%02db.x"):
  slot 0   00.x   [bzip] frames 0 (xform 0)  meshes 2  verts 112  tris 150  materials 2
  slot 1   01.x   [bin ] frames 0 (xform 0)  meshes 5  verts 19153  tris 11315  materials 40  vcolor 5
  ...
  totals: plain 27 meshes / 28372 tris;  b-variant 26 meshes / 28148 tris
Collision meshes (always loaded):
           collision.x            [txt ] ... meshes 1  verts 488  tris 503
           collision_camera.x     [bzip] ... meshes 13  verts 459  tris 299
           collision_tama.x       [bzip] ... meshes 1  verts 453  tris 310
Textures referenced by visual meshes (85 unique):
  kumo.bmp  -> data/stage01/kumo.cnv ok
  ...
  0 missing
```

## Validation done
- Built and run against the real `daybreak00.dat` (a Windows llvm-mingw build was used while WSL had no compiler): all 12 stages parse with 0 errors.
- Text-format `collision.x` vertex count (488) cross-checked against an independent regex read.
- MSZIP layout cross-checked against Python's zlib on the same data.
### WSL build verified (2026-11-20)
Toolchain: Ubuntu 26.04 (WSL), g++ 15.2.0, GNU Make 4.4.1, installed with `apt-get install -y g++ make`.
```sh
cd port && make        # g++ -std=c++17 -O2 -Wall -Wextra ... → build/stageinfo; builds clean, no warnings
build/stageinfo "/path/to/Higurashi Daybreak (Japan)/data/daybreak00.dat" 1
```
Stage 01, WSL compared with the Windows (llvm-mingw clang++) build of the same source:

| | Windows | WSL |
|---|---|---|
| plain set | 27 meshes / 28 372 tris | 27 meshes / 28 372 tris |
| `b` set | 26 meshes / 28 148 tris | 26 meshes / 28 148 tris |
| texture references | 85 unique, 85 resolved | 85 unique, 85 resolved |
| output lines | 131 | 131 |

The raw text differs on 164 lines, all of which are **column padding only**: `printf("%-28s")` pads by bytes. The Windows build has no iconv and prints raw Shift-JIS (2 bytes per Japanese character), while the WSL build converts names to UTF-8 (3 bytes per character), so the columns shift. After normalising whitespace the outputs are identical. They are also identical for all 12 stages (1–6, 11–15, 21).

### ARM64 Linux build under emulation (2026-11-20)
No Apple/Xcode environment was available (Windows host + WSL only), so **iOS was not built**. Tested target: **aarch64 Linux (glibc), cross-compiled in WSL and run under QEMU user-mode**.
```sh
sudo apt install -y g++-aarch64-linux-gnu qemu-user          # cross g++ 15.2.0, qemu-aarch64 10.2.1
cd port && make CXX=aarch64-linux-gnu-g++ BIN=build/stageinfo-arm64   # clean, no warnings
qemu-aarch64 -L /usr/aarch64-linux-gnu build/stageinfo-arm64 "<…>/daybreak00.dat" 1
```
| stage 01 | x86-64 (WSL) | aarch64 (QEMU) |
|---|---|---|
| plain set | 27 meshes / 28 372 tris | 27 meshes / 28 372 tris |
| `b` set | 26 meshes / 28 148 tris | 26 meshes / 28 148 tris |
| texture references | 85, all resolved | 85, all resolved (0 missing) |

All 12 stages give the same content as x86-64. The only visible difference is name encoding: the cross sysroot ships no glibc `gconv` modules (`/usr/aarch64-linux-gnu/lib/gconv` is absent), so `iconv_open("UTF-8","CP932")` fails under QEMU. The reader then prints raw Shift-JIS names, the same fallback the Windows build uses, and the aarch64 output is byte-identical to the Windows build for every stage. On a real ARM64 Linux system with `libc6` gconv modules installed, the names would print as UTF-8.

Not yet covered: an iOS build. iOS uses Apple's libiconv rather than glibc, so whether `CP932` is accepted there is untested; the reader still works without it, printing raw names. Building for iOS needs macOS with Xcode, e.g. `xcrun -sdk iphoneos clang++ -arch arm64 -std=c++17 src/*.cpp`.

### Refactor for the iOS app (2026-11-20)
The analysis code moved from `stageinfo.cpp` into `stage.cpp` so the CLI and the C API share it. Regression check: `build/stageinfo` output is **byte-identical** to the pre-refactor WSL output for all 12 stages. `build/capi_test` reports PASS on x86-64 and on aarch64 under QEMU (`make CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc BIN=build/arm64/stageinfo CAPI=build/arm64/capi_test build/arm64/capi_test`).

### Kai overlay (2026-11-21)
`ArchiveSet` (`src/archive.*`) searches attached archives in order. `Archive`/`ArchiveSet` share a `Source` interface, so the stage code and CLI take either. C API: `db_stage_report_open_set(paths, count, ...)`, paths in search order. `db_stage_report_open` is now a one-archive wrapper.

Regression (WSL g++ 15.2.0, base `daybreak00.dat` only): `stageinfo` output is **byte-identical** to the previous build for all 12 stages, with and without `--tree`, plus `--stages` and `--x`. `capi_test` passes the retail and fixture expectations. `make test` passes all 16 checks, with no compiler warnings.

Against Kai `daybreak04.dat` (4434 entries, copied out of the disc image to a scratch folder, never into the repo):

| stage | base 00 only | Kai 04 → 00 |
|---|---|---|
| 06 | 8 meshes / 11 247 tris, no `b` set, 20 textures, **12 missing** | 40 / 23 961 (both sets), 38 textures, 0 missing, 0 errors |
| 16 | absent | 40 / 23 961 (both sets), 38 textures, 0 missing, 0 errors |
| 01–05, 11–15, 21 | — | unchanged |

Stages 05 and 15 are missing 4 textures with or without the overlay. Not yet covered: the iOS app still opens one archive; BGM (`daybreak05`) and movies (`02`/`06`) are separate in Kai and are not part of this search.

## Port notes (what this means for a Metal renderer)
See `analysis/05-stage-loading.md`. In short: stage geometry is plain world-space meshes with per-vertex colour, normals and UVs. Each slot has a fixed render recipe (opaque / multiplicative / alpha-tested + point-sampled / sky). `material.Power != 0` means additive. D3D is left-handed with CCW culling.
