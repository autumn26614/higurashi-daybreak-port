/* Runtime for statically recompiled 32-bit x86 code (see recomp/README.md).
 *
 * Every guest function becomes `void F_xxxxxxxx(CPU *c)`. Inside a function the eight
 * general registers and the flags live in locals (so the host compiler keeps them in
 * registers); they are written back to the CPU (OUT) around calls, indirect jumps and
 * returns, and read again (IN) after a call. Flags are assumed dead across calls and at
 * function entry, which holds for compiler-generated code. Guest memory is one flat block:
 * guest address a is host address GM + a. The x87 stack is kept as doubles. */
#ifndef RECOMP_X86_H
#define RECOMP_X86_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t r[8];          /* eax ecx edx ebx esp ebp esi edi */
    double st[8];           /* x87 registers, physical order */
    uint32_t top;           /* x87 stack top (physical index of ST(0)) */
    uint16_t fsw, fcw;      /* x87 status (condition bits only) and control word */
    uint32_t fs_base;       /* guest address of the thread information block */
    void *thread;           /* host per-thread data for the shims */
} CPU;

typedef void (*GuestFn)(CPU *);
typedef struct FnEntry { uint32_t addr; GuestFn fn; } FnEntry;
typedef struct ImportSlot { uint32_t iat; const char *dll, *name; } ImportSlot;
typedef struct ExportSlot { const char *name; uint32_t ordinal, addr; } ExportSlot;

extern uint8_t *GM;         /* guest memory base */

/* Provided by the runtime (runtime.c). */
void rt_icall(CPU *c, uint32_t target);                 /* call/jump to a computed guest address */
void rt_trap(CPU *c, uint32_t addr, const char *what);  /* unsupported or undecodable code */
void rt_cpuid(CPU *c);                                  /* reports a CPU without MMX/SSE/3DNow! */
void rt_rdtsc(CPU *c);

/* ---- memory ---- */
static inline uint32_t ld8(uint32_t a) { return GM[a]; }
static inline uint32_t ld16(uint32_t a) { uint16_t v; memcpy(&v, GM + a, 2); return v; }
static inline uint32_t ld32(uint32_t a) { uint32_t v; memcpy(&v, GM + a, 4); return v; }
static inline uint64_t ld64(uint32_t a) { uint64_t v; memcpy(&v, GM + a, 8); return v; }
static inline void st8(uint32_t a, uint32_t v) { GM[a] = (uint8_t)v; }
static inline void st16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(GM + a, &x, 2); }
static inline void st32(uint32_t a, uint32_t v) { memcpy(GM + a, &v, 4); }
static inline void st64(uint32_t a, uint64_t v) { memcpy(GM + a, &v, 8); }
static inline double ldf32(uint32_t a) { float v; memcpy(&v, GM + a, 4); return v; }
static inline double ldf64(uint32_t a) { double v; memcpy(&v, GM + a, 8); return v; }
static inline void stf32(uint32_t a, double v) { float x = (float)v; memcpy(GM + a, &x, 4); }
static inline void stf64(uint32_t a, double v) { memcpy(GM + a, &v, 8); }
double ldf80(uint32_t a);
void stf80(uint32_t a, double v);

/* ---- flags ---- */
typedef struct Fl {
    uint8_t cf, zf, sf, of;
    uint8_t pr;             /* low byte of the last result, for PF */
} Fl;

#define XM(w) ((w) == 4 ? 0xFFFFFFFFu : (w) == 2 ? 0xFFFFu : 0xFFu)
#define XS(w) ((w) == 4 ? 0x80000000u : (w) == 2 ? 0x8000u : 0x80u)
#define XB(w) ((w) * 8)

static inline void fl_res(Fl *f, uint32_t r, int w) {
    f->zf = (r & XM(w)) == 0;
    f->sf = (r & XS(w)) != 0;
    f->pr = (uint8_t)r;
}
static inline uint32_t x_logic(Fl *f, uint32_t r, int w) {
    r &= XM(w); f->cf = 0; f->of = 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_add(Fl *f, uint32_t a, uint32_t b, int w) {
    uint32_t m = XM(w); a &= m; b &= m;
    uint32_t r = (a + b) & m;
    f->cf = r < a; f->of = (((a ^ r) & (b ^ r)) & XS(w)) != 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_adc(Fl *f, uint32_t a, uint32_t b, int w) {
    uint32_t m = XM(w); a &= m; b &= m;
    uint64_t wide = (uint64_t)a + b + f->cf;
    uint32_t r = (uint32_t)wide & m;
    f->cf = wide > m; f->of = (((a ^ r) & (b ^ r)) & XS(w)) != 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_sub(Fl *f, uint32_t a, uint32_t b, int w) {
    uint32_t m = XM(w); a &= m; b &= m;
    uint32_t r = (a - b) & m;
    f->cf = a < b; f->of = (((a ^ b) & (a ^ r)) & XS(w)) != 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_sbb(Fl *f, uint32_t a, uint32_t b, int w) {
    uint32_t m = XM(w); a &= m; b &= m;
    uint32_t cin = f->cf;
    uint32_t r = (a - b - cin) & m;
    f->cf = (uint64_t)a < (uint64_t)b + cin; f->of = (((a ^ b) & (a ^ r)) & XS(w)) != 0; fl_res(f, r, w);
    return r;
}
static inline uint32_t x_inc(Fl *f, uint32_t a, int w) {
    uint32_t r = (a + 1) & XM(w); f->of = r == XS(w); fl_res(f, r, w); return r;
}
static inline uint32_t x_dec(Fl *f, uint32_t a, int w) {
    uint32_t r = (a - 1) & XM(w); f->of = (a & XM(w)) == XS(w); fl_res(f, r, w); return r;
}
static inline uint32_t x_neg(Fl *f, uint32_t a, int w) {
    a &= XM(w); uint32_t r = (0u - a) & XM(w); f->cf = a != 0; f->of = a == XS(w); fl_res(f, r, w); return r;
}
/* Shifts take the count already masked to 0..31; a zero count leaves value and flags. */
static inline uint32_t x_shl(Fl *f, uint32_t a, uint32_t n, int w) {
    if (!n) return a & XM(w);
    a &= XM(w);
    uint32_t r = n >= 32 ? 0 : (a << n) & XM(w);
    f->cf = n <= (uint32_t)XB(w) ? (uint8_t)((a >> (XB(w) - n)) & 1) : 0;
    f->of = ((r & XS(w)) != 0) ^ f->cf; fl_res(f, r, w); return r;
}
static inline uint32_t x_shr(Fl *f, uint32_t a, uint32_t n, int w) {
    if (!n) return a & XM(w);
    a &= XM(w);
    uint32_t r = a >> n;
    f->cf = (a >> (n - 1)) & 1; f->of = (a & XS(w)) != 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_sar(Fl *f, uint32_t a, uint32_t n, int w) {
    if (!n) return a & XM(w);
    int32_t s = w == 4 ? (int32_t)a : w == 2 ? (int16_t)a : (int8_t)a;
    uint32_t r = (uint32_t)(s >> (n > 31 ? 31 : n)) & XM(w);
    f->cf = (uint8_t)((s >> (n - 1 > 31 ? 31 : n - 1)) & 1); f->of = 0; fl_res(f, r, w); return r;
}
static inline uint32_t x_rol(Fl *f, uint32_t a, uint32_t n, int w) {
    a &= XM(w);
    if (!n) return a;
    n %= XB(w);
    uint32_t r = n ? ((a << n) | (a >> (XB(w) - n))) & XM(w) : a;
    f->cf = r & 1; f->of = ((r & XS(w)) != 0) ^ f->cf; return r;
}
static inline uint32_t x_ror(Fl *f, uint32_t a, uint32_t n, int w) {
    a &= XM(w);
    if (!n) return a;
    n %= XB(w);
    uint32_t r = n ? ((a >> n) | (a << (XB(w) - n))) & XM(w) : a;
    f->cf = (r & XS(w)) != 0; f->of = ((r >> (XB(w) - 1)) ^ (r >> (XB(w) - 2))) & 1; return r;
}
static inline uint32_t x_rcl(Fl *f, uint32_t a, uint32_t n, int w) {
    a &= XM(w);
    n %= XB(w) + 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = (a & XS(w)) != 0;
        a = ((a << 1) | f->cf) & XM(w);
        f->cf = (uint8_t)out;
    }
    if (n) f->of = ((a & XS(w)) != 0) ^ f->cf;
    return a;
}
static inline uint32_t x_rcr(Fl *f, uint32_t a, uint32_t n, int w) {
    a &= XM(w);
    n %= XB(w) + 1;
    if (n) f->of = ((a & XS(w)) != 0) ^ f->cf;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = a & 1;
        a = (a >> 1) | (f->cf ? XS(w) : 0);
        f->cf = (uint8_t)out;
    }
    return a;
}
static inline uint32_t x_shld(Fl *f, uint32_t a, uint32_t b, uint32_t n) {
    if (!n) return a;
    uint32_t r = (a << n) | (b >> (32 - n));
    f->cf = (a >> (32 - n)) & 1; f->of = ((r ^ a) >> 31) & 1; fl_res(f, r, 4); return r;
}
static inline uint32_t x_shrd(Fl *f, uint32_t a, uint32_t b, uint32_t n) {
    if (!n) return a;
    uint32_t r = (a >> n) | (b << (32 - n));
    f->cf = (a >> (n - 1)) & 1; f->of = ((r ^ a) >> 31) & 1; fl_res(f, r, 4); return r;
}
/* Two/three operand imul (truncating). */
static inline uint32_t x_imul(Fl *f, uint32_t a, uint32_t b, int w) {
    int64_t p;
    if (w == 4) p = (int64_t)(int32_t)a * (int32_t)b;
    else p = (int64_t)(int16_t)a * (int16_t)b;
    uint32_t r = (uint32_t)p & XM(w);
    int64_t back = w == 4 ? (int64_t)(int32_t)r : (int64_t)(int16_t)r;
    f->cf = f->of = back != p; fl_res(f, r, w); return r;
}

static inline uint8_t fl_pf(const Fl *f) { return !__builtin_parity(f->pr); }
static inline uint32_t fl_pack(const Fl *f) {
    return f->cf | (fl_pf(f) << 2) | (f->zf << 6) | (f->sf << 7) | ((uint32_t)f->of << 11) | 0x202;
}
static inline void fl_unpack(Fl *f, uint32_t v) {
    f->cf = v & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->of = (v >> 11) & 1;
    f->pr = (v & 4) ? 0 : 1;  /* a byte whose parity gives the stored PF */
}

/* Condition codes. */
#define CC_O(f)  ((f).of)
#define CC_NO(f) (!(f).of)
#define CC_B(f)  ((f).cf)
#define CC_AE(f) (!(f).cf)
#define CC_E(f)  ((f).zf)
#define CC_NE(f) (!(f).zf)
#define CC_BE(f) ((f).cf || (f).zf)
#define CC_A(f)  (!(f).cf && !(f).zf)
#define CC_S(f)  ((f).sf)
#define CC_NS(f) (!(f).sf)
#define CC_P(f)  (fl_pf(&(f)))
#define CC_NP(f) (!fl_pf(&(f)))
#define CC_L(f)  ((f).sf != (f).of)
#define CC_GE(f) ((f).sf == (f).of)
#define CC_LE(f) ((f).zf || (f).sf != (f).of)
#define CC_G(f)  (!(f).zf && (f).sf == (f).of)

/* ---- x87 ---- */
#define ST(i) (c->st[(c->top + (i)) & 7])
#define FPUSH(v) do { double v_ = (v); c->top = (c->top - 1) & 7; c->st[c->top] = v_; } while (0)
#define FPOP() (c->top = (c->top + 1) & 7)

/* Compare a with b into C0/C2/C3 (as fcom does). */
static inline void fcmp(CPU *c, double a, double b) {
    uint16_t s = c->fsw & ~0x4500;
    if (a != a || b != b) s |= 0x4500;
    else if (a < b) s |= 0x0100;
    else if (a == b) s |= 0x4000;
    c->fsw = s;
}
/* fcomi/fucomi: compare into ZF/PF/CF. */
static inline void fcmpi(Fl *f, double a, double b) {
    f->of = 0; f->sf = 0;
    if (a != a || b != b) { f->zf = 1; f->cf = 1; f->pr = 0; }
    else { f->zf = a == b; f->cf = a < b; f->pr = 1; }
}
static inline uint32_t fsw_read(const CPU *c) { return (c->fsw & ~0x3800u) | ((c->top & 7) << 11); }
/* Round to an integer by the control word's rounding mode. */
static inline double fround(const CPU *c, double x) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(x);
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
static inline int64_t fist(const CPU *c, double x, int64_t lo, int64_t hi, int64_t indefinite) {
    double r = fround(c, x);
    if (!(r >= (double)lo && r <= (double)hi)) return indefinite;
    return (int64_t)r;
}
static inline void fxam(CPU *c) {
    double x = ST(0);
    uint16_t s = c->fsw & ~0x4700;
    if (signbit(x)) s |= 0x0200;
    if (x != x) s |= 0x0100;
    else if (isinf(x)) s |= 0x0500;
    else if (x == 0) s |= 0x4000;
    else if (fpclassify(x) == FP_SUBNORMAL) s |= 0x4400;
    else s |= 0x0400;
    c->fsw = s;
}

/* Register locals of a translated function, and their exchange with the CPU. */
#define REGS uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], \
    esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7]; Fl f = {0, 0, 0, 0, 0}; uint8_t df = 0; (void)f; (void)df
#define OUT (c->r[0] = eax, c->r[1] = ecx, c->r[2] = edx, c->r[3] = ebx, \
    c->r[4] = esp, c->r[5] = ebp, c->r[6] = esi, c->r[7] = edi)
#define IN (eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3], \
    esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7])

#ifdef __cplusplus
}
#endif
#endif
