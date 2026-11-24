// Host side of the recompiled program: guest memory, loader, import thunks, threads and the
// helpers shims use. See recomp/README.md.
#pragma once

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "x86.h"

namespace rt {

// ---- guest address space ----
constexpr uint32_t kThunkBase = 0xFFF00000;  // import and COM method thunks (not memory)
constexpr uint32_t kHeapBase = 0x01000000;
constexpr uint32_t kHeapEnd = 0x78000000;
constexpr uint32_t kMainStack = 8u << 20;
constexpr uint32_t kThreadStack = 1u << 20;

void memory_init();
uint32_t galloc(uint32_t size, bool zero = false);  // guest heap (16-byte aligned)
void gfree(uint32_t a);
uint32_t grealloc(uint32_t a, uint32_t size);
uint32_t gsize(uint32_t a);
uint32_t gstrdup(const std::string &s);

inline uint32_t arg(CPU *c, int i) { return ld32(c->r[4] + 4 + 4 * i); }
inline const char *gstr(uint32_t a) { return a ? reinterpret_cast<const char *>(GM + a) : nullptr; }
inline char *gptr(uint32_t a) { return a ? reinterpret_cast<char *>(GM + a) : nullptr; }
inline uint32_t gaddr(const void *p) { return uint32_t(reinterpret_cast<const uint8_t *>(p) - GM); }

// ---- shims ----
// A shim reads its arguments with arg(c, i) (thiscall: `this` in c->r[1]) and returns eax.
// stack_bytes is the stdcall argument size it pops, or kCdecl.
using ShimFn = uint32_t (*)(CPU *c);
constexpr int kCdecl = -1;

struct ShimReg {
    ShimReg(const char *dll, const char *name, int stack_bytes, ShimFn fn);
};
struct DataReg {  // an imported variable: the IAT slot gets the address `make` returns
    DataReg(const char *dll, const char *name, uint32_t (*make)());
};

#define RT_CAT2(a, b) a##b
#define RT_CAT(a, b) RT_CAT2(a, b)
// SHIM(dll, name, stack bytes) { ... return eax; }   (name must be an identifier)
#define SHIM(dll, name, bytes) SHIM_AS(dll, name, #name, bytes)
// SHIM_AS(dll, ident, "exported name", bytes) for decorated C++ names
#define SHIM_AS(dll, ident, exported, bytes)                                                   \
    static uint32_t RT_CAT(shim_##dll##_, ident)(CPU * c);                                      \
    static ::rt::ShimReg RT_CAT(reg_##dll##_, ident)(#dll, exported, bytes,                     \
                                                     RT_CAT(shim_##dll##_, ident));             \
    static uint32_t RT_CAT(shim_##dll##_, ident)(CPU * c)

// The guest address GetProcAddress gives for dll!name (0 if there is no shim).
uint32_t resolve_import(const std::string &dll, const std::string &name);

// A thunk for a host function (COM methods, callbacks given to the guest).
uint32_t make_thunk(const std::string &name, int stack_bytes, ShimFn fn);

// ---- calling guest code ----
// Calls guest function `fn` with 32-bit stack arguments; returns eax. Works for cdecl and
// stdcall (the stack pointer is restored either way); `ecx` is set for thiscall.
uint32_t call_guest(CPU *c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t ecx = 0);

// ---- threads ----
CPU *current();
CPU *new_cpu(uint32_t stack_size);   // CPU with its own guest stack and TIB
void free_cpu(CPU *c);
uint32_t thread_id();

// ---- program ----
struct Program {
    const FnEntry *table;
    uint32_t count;
    const ImportSlot *imports;
    uint32_t import_count;
};
// Maps `exe` (path on the host) into guest memory and binds its imports.
uint32_t load_exe(const std::string &exe, const Program &p);  // returns the entry point
GuestFn lookup(uint32_t addr);

[[noreturn]] void fatal(const char *fmt, ...);
void log(const char *fmt, ...);
extern bool trace_imports;

}  // namespace rt
