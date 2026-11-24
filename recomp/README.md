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
make GEN=/tmp/rgen BUILD=/tmp/rbuild -j12    # translate, then compile (about a minute)
/tmp/rbuild/daybreak-recomp "<folder with daybreak.exe>"
RECOMP_TRACE=1 ...                           # log every import call
```

Needs Python 3 with Capstone 5 (`~/.local/daybreak-py` is searched).

## Status

- 4,401 functions are translated. Untranslated instructions fall into the few `int3`
  paddings and data that only looks like code.
- The C runtime, kernel32, user32, gdi32 handles, winmm (timers, mmio), imm32 and
  ole32/oleaut32 are shimmed. Startup runs through the CRT, the game's static constructors,
  window creation and COM initialisation, and reaches `Direct3DCreate9`.

## Next

1. Direct3D 9 (`IDirect3D9`, `IDirect3DDevice9`, textures, buffers) on a recording backend,
   then on Metal. Also the D3DX functions the game imports: math, `D3DXLoadMeshHierarchyFromXInMemory`,
   `D3DXCreateEffect`.
2. DirectInput 8 (keyboard and pad), DirectSound (buffers, 3D), DirectShow for the movies.
3. GDI text: `GetGlyphOutlineA` on CoreText.
4. iOS: a smaller guest reservation, a game thread with a large stack, Metal presentation,
   touch → DirectInput.
