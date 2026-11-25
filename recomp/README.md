# Static recompilation of daybreak.exe

The game's own code, translated ahead of time from 32-bit x86 to C, compiled for the host
(ARM64 on iOS), and run on top of replacements for the Windows APIs it calls. iOS forbids
JIT, so every function is translated before the build; nothing is interpreted.

Nothing here contains game code. `tools/lift.py` reads **your** `daybreak.exe` and writes the
C into `gen/` (git-ignored); the build compiles that together with `runtime/`.

## Layout

| Path | What |
|---|---|
| `tools/pe.py` | PE32 reader: sections, imports, exports, relocations |
| `tools/discover.py` | Finds the code: recursive descent from the entry, direct call targets, code addresses in data and in instruction immediates (the exe has no relocations), MSVC jump tables. Prints an instruction survey |
| `tools/lift.py` | x86 → C, one C function per guest function |
| `runtime/x86.h` | What the generated C needs: memory access, flags, x87 |
| `runtime/runtime.*` | Guest memory, loader, import thunks, dispatcher, heap, threads |
| `runtime/host.*` | Windows paths onto the game folder, kernel handles, `printf` over guest arguments |
| `runtime/com.cpp`, `win.h` | Host objects exposed to the guest as COM objects |
| `runtime/shims_*.cpp` | The imported APIs: C runtime, kernel32, user32/gdi32/winmm/imm32/ole32 |

## How the translation works

- **Registers and flags** are locals in each translated function (`REGS`), so the host
  compiler keeps them in registers. They are written back to the `CPU` struct (`OUT`) before
  calls, indirect jumps and returns, and read again (`IN`) after a call. Flags are
  computed eagerly by small inline helpers (`x_add`, `x_sub`, ...). The compiler drops the
  ones that are never read. Flags are taken to be dead across calls, which holds for
  compiled code.
- **Memory** is one flat block: guest address `a` is host `GM + a` (4 GiB reserved).
  The exe is mapped at its preferred base 0x400000.
- **Calls** push the real return address on the guest stack (code such as `__SEH_prolog`
  and `_chkstk` moves it around) and then call the translated function in C. `ret` pops
  it and returns in C. A direct jump to another function's entry becomes a tail call.
- **Indirect calls** (virtual methods, function pointers, imports) go through `rt_icall`,
  which looks the address up in the generated table or finds the import thunk.
- **Imports** are bound to host shims at load time. Every IAT slot holds a thunk address
  (0xFFF00000 and up). A shim reads its arguments from the guest stack and pops them if
  stdcall. Imports without a shim stop the program with their name when first called.
- **COM** (Direct3D 9, DirectInput 8, DirectSound, DirectShow): guest objects whose vtables
  hold thunks to host methods.
- **x87** is modelled with doubles. Instructions are decoded from their opcode bytes,
  because disassemblers disagree on the names of the reversed forms (`fsubr` and `fsubrp`).

## Build and run (Linux/WSL, headless)

```sh
make GEN=/tmp/rgen BUILD=/tmp/rbuild -j8     # translate both modules, then compile
/tmp/rbuild/daybreak-recomp "<folder with daybreak.exe and d3dx9_25.dll>"
tools/run_shots.sh <seconds> <every> <first> [keys]   # PNG frames into /tmp/shots
SKIP=1 tools/run_shots.sh 250 600 3000 "$(tools/battle_keys.sh)"   # title to a battle
```

Keep the game folder on a Linux file system: not WSL's `/mnt/c` (slow), and not `/tmp` (a RAM disk there). Needs Python 3 with
Capstone 5 (`~/.local/daybreak-py` is searched).

Kai (the expansion) is a separate executable, translated with the prefix K next to the base
game's F: `make GAME=<folder> KAI=<folder with DaybreakDX.exe> GEN=... BUILD=...` builds
both, and `RECOMP_GAME=kai` runs Kai (the iOS app asks which). It needs `daybreak04.dat`
to `daybreak06.dat` besides the base archives. Its movies go through a DirectShow renderer
of its own that draws into a texture; the graph connects to it and hands it RGB24 samples.

Environment switches:

| Variable | Effect |
|---|---|
| `RECOMP_SHOTS=dir:every[:first]` | software renderer; writes every Nth frame as PNG |
| `RECOMP_KEYS=600:1c:down,606:1c:up,t40:1c:down` | scripted DirectInput keys by frame, or by seconds (`t`) |
| `RECOMP_SKIP_MOVIES=1` | movies play 200× fast without sound |
| `RECOMP_TRACE=1` | log every import call with its return address |
| `RECOMP_DRAWLOG=<frame>` | describe every draw of that frame |
| `RECOMP_SAMPLE=<seconds>` | every guest thread prints its host backtrace that often |
| `RECOMP_PROTECT=<address>` | make a guest page read-only and report who writes it |
| `RECOMP_NOVSYNC=1` | do not pace Present to 60 Hz |
| `RECOMP_FPS=1` | log the frame rate and draws per frame every 600 frames |
| `RECOMP_PAD=1` | a connected game controller; scripted keys 1000+ press its buttons |

## Tests

- `make test` (x86-64 Linux): `tests/flags_test.cpp` checks every flag helper against the
  CPU; `tests/lifter/run.sh` compiles C as 32-bit x86 at -O0..-Os, translates it and runs it
  against native builds (integer maths, jump tables, recursion, strings, x87).
- `tests/render_test.mm` (macOS, in CI): synthetic Direct3D 9 scenes through the shims, with
  the software renderer and with Metal, compared pixel by pixel; the frames are kept as a CI
  artifact. `tools/msl_check.cpp`: the generated Metal code, including shaders that use every
  instruction the game's shaders use, compiled with Apple's compiler.

## Status

- `daybreak.exe` (~4,400 functions) and `d3dx9_25.dll` (~6,000) are translated. D3DX runs
  as Microsoft wrote it (effects, .X parsing, meshes, animation); `cpuid` reports a CPU
  without SIMD so it stays on its x87 paths.
- Shimmed: the C runtime, kernel32 (threads, events, semaphores, fair critical
  sections, files), user32, gdi32 (fonts through a glyph interface), winmm (timers, mmio),
  imm32, ole32/oleaut32, advapi32, Direct3D 9 (over guest memory, with a renderer
  interface), DirectInput 8 (keyboard), DirectSound 8 (software mixer, 3D, notifications),
  DirectShow (the game's movies: its own source filter, pl_mpeg decoding, IVideoWindow).
- Running headless with the software renderer: opening movie → title → story intro movie
  → team select → weapon select → talk scene → battle, all drawn by the game's own code. Kai
  gets as far: its opening movie, title, story team select and a battle.

## iOS

`ios/` is the app: the translated game in a CAMetalLayer view (`runtime/metal.mm`, shaders
translated to Metal by `runtime/msl.cpp`), touch controls on the game's keyboard keys,
sound through AVAudioEngine, glyphs from CoreText. On a Mac:

```sh
pip3 install capstone && brew install xcodegen
recomp/ios/build.sh "<folder with daybreak.exe and d3dx9_25.dll>"   # translates into recomp/gen
open recomp/ios/DaybreakDX.xcodeproj                            # set a signing team, run
```

Copy the game's data folder (daybreak.exe, d3dx9_25.dll, daybreak00–02.dat) into the app's
Documents (Files › On My iPhone › Daybreak DX). CI (`.github/workflows/recomp.yml`)
builds everything without game data: the runtime for macOS and the simulator, the
generated Metal code with Apple's compiler, and the app (which then says the code is
missing).

## Next

1. Run on a device with the real game: check the Metal renderer against the software
   renderer's frames, then performance (the translated code is built -O2).
2. Game controllers (DirectInput joystick), saving to the app's Documents, the controls'
   layout per screen.
