// Checks the flag helpers of runtime/x86.h against the real CPU (x86-64 hosts only): every
// operation runs as an instruction through inline assembly and as the helper, on edge and
// random values, and results and defined flags must agree.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "x86.h"

uint8_t *GM = nullptr;
extern "C" void rt_icall(CPU *, uint32_t) {}
extern "C" void rt_trap(CPU *, uint32_t, const char *) {}
extern "C" void rt_cpuid(CPU *) {}
extern "C" void rt_rdtsc(CPU *) {}
extern "C" double ldf80(uint32_t) { return 0; }
extern "C" void stf80(uint32_t, double) {}

namespace {
constexpr uint32_t CF = 1, PF = 4, ZF = 0x40, SF = 0x80, OF = 0x800;
int failures = 0, checks = 0;

uint32_t flags_of(const Fl &f) {
    return (f.cf ? CF : 0) | (fl_pf(&f) ? PF : 0) | (f.zf ? ZF : 0) | (f.sf ? SF : 0) | (f.of ? OF : 0);
}

void check(const char *what, int w, uint32_t a, uint32_t b, uint32_t n, uint32_t got_r, uint32_t want_r, uint32_t got_f,
           uint32_t want_f, uint32_t mask) {
    checks++;
    uint32_t m = XM(w);
    if ((got_r & m) != (want_r & m) || (got_f & mask) != (want_f & mask)) {
        if (++failures <= 40)
            printf("FAIL %s/%d a=%08x b=%08x n=%u: result %08x want %08x, flags %03x want %03x (mask %03x)\n", what, w * 8,
                   a, b, n, got_r & m, want_r & m, got_f & mask, want_f & mask, mask);
    }
}

// Runs `insn` (AT&T, operands %0 = a in/out, %2 = b or count in cl) with the incoming
// carry `cin`, returning the result and the flags.
#define BINOP(name, suffix, reg_a, reg_b, T)                                                          \
    void hw_##name##suffix(uint32_t a, uint32_t b, uint32_t cin, uint32_t &r, uint32_t &fl) {         \
        T x = T(a), y = T(b);                                                                        \
        uint64_t flags;                                                                              \
        asm volatile("bt $0, %q3\n\t" #name #suffix " %" reg_b "2, %" reg_a "0\n\tpushfq\n\tpopq %1"  \
                     : "+r"(x), "=r"(flags)                                                          \
                     : "r"(y), "r"(uint64_t(cin))                                                    \
                     : "cc");                                                                        \
        r = uint32_t(x);                                                                             \
        fl = uint32_t(flags);                                                                        \
    }

#define BINOPS(name)                    \
    BINOP(name, l, "k", "k", uint32_t)  \
    BINOP(name, w, "w", "w", uint16_t)  \
    BINOP(name, b, "b", "b", uint8_t)
BINOPS(add)
BINOPS(adc)
BINOPS(sub)
BINOPS(sbb)
BINOPS(and)
BINOPS(xor)
BINOPS(or)

// Shifts and rotates by cl.
#define SHOP(name, suffix, reg_a, T)                                                                  \
    void hw_##name##suffix(uint32_t a, uint32_t n, uint32_t cin, uint32_t fin, uint32_t &r, uint32_t &fl) { \
        T x = T(a);                                                                                  \
        uint64_t flags, in = (uint64_t(fin) & 0x8D5) | 2;                                            \
        asm volatile("pushq %q3\n\tpopfq\n\tmovb %b2, %%cl\n\t" #name #suffix " %%cl, %" reg_a "0\n\tpushfq\n\tpopq %1" \
                     : "+r"(x), "=r"(flags)                                                          \
                     : "r"(uint64_t(n)), "r"(in | (cin & 1))                                         \
                     : "cc", "rcx");                                                                 \
        r = uint32_t(x);                                                                             \
        fl = uint32_t(flags);                                                                        \
    }
#define SHOPS(name)                \
    SHOP(name, l, "k", uint32_t)   \
    SHOP(name, w, "w", uint16_t)   \
    SHOP(name, b, "b", uint8_t)
SHOPS(shl)
SHOPS(shr)
SHOPS(sar)
SHOPS(rol)
SHOPS(ror)
SHOPS(rcl)
SHOPS(rcr)

#define UNOP(name, suffix, reg_a, T)                                                                  \
    void hw_##name##suffix(uint32_t a, uint32_t fin, uint32_t &r, uint32_t &fl) {                    \
        T x = T(a);                                                                                  \
        uint64_t flags;                                                                              \
        asm volatile("pushq %q2\n\tpopfq\n\t" #name #suffix " %" reg_a "0\n\tpushfq\n\tpopq %1"      \
                     : "+r"(x), "=r"(flags)                                                          \
                     : "r"((uint64_t(fin) & 0x8D5) | 2)                                              \
                     : "cc");                                                                        \
        r = uint32_t(x);                                                                             \
        fl = uint32_t(flags);                                                                        \
    }
#define UNOPS(name)                \
    UNOP(name, l, "k", uint32_t)   \
    UNOP(name, w, "w", uint16_t)   \
    UNOP(name, b, "b", uint8_t)
UNOPS(inc)
UNOPS(dec)
UNOPS(neg)

void hw_shld(uint32_t a, uint32_t b, uint32_t n, uint32_t &r, uint32_t &fl) {
    uint64_t flags;
    asm volatile("movb %b3, %%cl\n\tshldl %%cl, %k2, %k0\n\tpushfq\n\tpopq %1"
                 : "+r"(a), "=r"(flags)
                 : "r"(b), "r"(uint64_t(n))
                 : "cc", "rcx");
    r = a;
    fl = uint32_t(flags);
}
void hw_shrd(uint32_t a, uint32_t b, uint32_t n, uint32_t &r, uint32_t &fl) {
    uint64_t flags;
    asm volatile("movb %b3, %%cl\n\tshrdl %%cl, %k2, %k0\n\tpushfq\n\tpopq %1"
                 : "+r"(a), "=r"(flags)
                 : "r"(b), "r"(uint64_t(n))
                 : "cc", "rcx");
    r = a;
    fl = uint32_t(flags);
}
void hw_imul(uint32_t a, uint32_t b, uint32_t &r, uint32_t &fl) {
    uint64_t flags;
    asm volatile("imull %k2, %k0\n\tpushfq\n\tpopq %1" : "+r"(a), "=r"(flags) : "r"(b) : "cc");
    r = a;
    fl = uint32_t(flags);
}
void hw_imulw(uint32_t a, uint32_t b, uint32_t &r, uint32_t &fl) {
    uint16_t x = uint16_t(a), y = uint16_t(b);
    uint64_t flags;
    asm volatile("imulw %w2, %w0\n\tpushfq\n\tpopq %1" : "+r"(x), "=r"(flags) : "r"(y) : "cc");
    r = x;
    fl = uint32_t(flags);
}

std::vector<uint32_t> values(int w, std::mt19937 &rng) {
    std::vector<uint32_t> v = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000,
                               0xFFFFFFFF, 0xFFFFFFFE, 0x80000001, 0x55555555, 0xAAAAAAAA};
    for (int i = 0; i < 40; i++) v.push_back(rng());
    for (auto &x : v) x &= XM(w);
    return v;
}
}  // namespace

int main() {
    std::mt19937 rng(12345);
    const uint32_t all = CF | PF | ZF | SF | OF;
    for (int w : {1, 2, 4}) {
        auto vals = values(w, rng);
        for (uint32_t a : vals)
            for (uint32_t b : vals)
                for (uint32_t cin : {0u, 1u}) {
                    uint32_t r, f;
                    Fl fl{};
#define TRY(op, helper, carry)                                                                      \
    {                                                                                               \
        if (w == 4) hw_##op##l(a, b, cin, r, f);                                                    \
        else if (w == 2) hw_##op##w(a, b, cin, r, f);                                               \
        else hw_##op##b(a, b, cin, r, f);                                                           \
        fl = Fl{};                                                                                  \
        fl.cf = uint8_t(cin);                                                                       \
        uint32_t got = helper;                                                                      \
        check(#op, w, a, b, cin, got, r, flags_of(fl), f, all);                                     \
    }
                    TRY(add, x_add(&fl, a, b, w), 0)
                    TRY(adc, x_adc(&fl, a, b, w), 1)
                    TRY(sub, x_sub(&fl, a, b, w), 0)
                    TRY(sbb, x_sbb(&fl, a, b, w), 1)
                    TRY(and, x_logic(&fl, a & b, w), 0)
                    TRY(xor, x_logic(&fl, a ^ b, w), 0)
                    TRY(or, x_logic(&fl, a | b, w), 0)
                }
        for (uint32_t a : vals)
            for (uint32_t n = 0; n < 34; n++)
                for (uint32_t cin : {0u, 1u}) {
                    uint32_t fin = cin ? 0x8D5 : 0;  // incoming flags (unchanged for count 0)
                    uint32_t r, f;
                    Fl fl{};
                    uint32_t bits = uint32_t(8 * w);
                    uint32_t cnt = n & 31;
#define SH(op, helper, mask)                                                                          \
    {                                                                                                 \
        if (w == 4) hw_##op##l(a, n, cin, fin, r, f);                                                 \
        else if (w == 2) hw_##op##w(a, n, cin, fin, r, f);                                            \
        else hw_##op##b(a, n, cin, fin, r, f);                                                        \
        fl_unpack(&fl, fin | (cin & 1));                                                              \
        uint32_t got = helper;                                                                        \
        check(#op, w, a, 0, n, got, r, flags_of(fl), f, mask);                                        \
    }
                    // Shifts: OF is defined for a count of 1; CF undefined past the width.
                    uint32_t shmask = cnt == 0 ? all : (CF * (cnt <= bits)) | PF | ZF | SF | (cnt == 1 ? OF : 0);
                    if (w != 4 && cnt > bits) shmask &= ~CF;
                    SH(shl, x_shl(&fl, a, cnt, w), shmask)
                    SH(shr, x_shr(&fl, a, cnt, w), shmask)
                    SH(sar, x_sar(&fl, a, cnt, w), (shmask | (cnt > bits ? CF : 0)) & ~(cnt > bits ? 0 : 0))
                    // Rotates touch only CF and OF (OF for a count of 1).
                    uint32_t rmask = cnt == 0 ? CF | OF : CF | (cnt == 1 ? OF : 0);
                    SH(rol, x_rol(&fl, a, cnt, w), rmask)
                    SH(ror, x_ror(&fl, a, cnt, w), rmask)
                    SH(rcl, x_rcl(&fl, a, cnt, w), rmask)
                    SH(rcr, x_rcr(&fl, a, cnt, w), rmask)
                }
        for (uint32_t a : vals)
            for (uint32_t fin : {0u, 0x8D5u}) {
                uint32_t r, f;
                Fl fl{};
#define UN(op, helper)                                                                              \
    {                                                                                               \
        if (w == 4) hw_##op##l(a, fin, r, f);                                                       \
        else if (w == 2) hw_##op##w(a, fin, r, f);                                                  \
        else hw_##op##b(a, fin, r, f);                                                              \
        fl_unpack(&fl, fin);                                                                        \
        uint32_t got = helper;                                                                      \
        check(#op, w, a, 0, fin, got, r, flags_of(fl), f, all);                                     \
    }
                UN(inc, x_inc(&fl, a, w))
                UN(dec, x_dec(&fl, a, w))
                UN(neg, x_neg(&fl, a, w))
            }
    }
    auto vals = values(4, rng);
    for (uint32_t a : vals)
        for (uint32_t b : vals) {
            uint32_t r, f;
            Fl fl{};
            for (uint32_t n = 0; n < 32; n++) {
                hw_shld(a, b, n, r, f);
                fl = Fl{};
                uint32_t got = x_shld(&fl, a, b, n);
                check("shld", 4, a, b, n, got, r, flags_of(fl), f, n == 0 ? 0 : CF | PF | ZF | SF | (n == 1 ? OF : 0));
                hw_shrd(a, b, n, r, f);
                fl = Fl{};
                got = x_shrd(&fl, a, b, n);
                check("shrd", 4, a, b, n, got, r, flags_of(fl), f, n == 0 ? 0 : CF | PF | ZF | SF | (n == 1 ? OF : 0));
            }
            hw_imul(a, b, r, f);
            fl = Fl{};
            uint32_t got = x_imul(&fl, a, b, 4);
            check("imul", 4, a, b, 0, got, r, flags_of(fl), f, CF | OF);
            hw_imulw(a, b, r, f);
            fl = Fl{};
            got = x_imul(&fl, a, b, 2);
            check("imul", 2, a, b, 0, got, r, flags_of(fl), f, CF | OF);
        }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
