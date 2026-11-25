/* Test functions for the lifter's differential test: compiled as 32-bit x86 (gcc -m32,
 * x87 floating point), translated by tools/lift.py and run, and compiled natively; the
 * results must agree. Each case is `uint32_t name(uint32_t a, uint32_t b)`. */
#include <stdint.h>

#define CASE(name) __attribute__((noinline, used)) uint32_t name(uint32_t a, uint32_t b)

static volatile uint32_t sink;
static uint32_t table[64];
static char buf[256];

CASE(c_arith) { return (a + b) * 3 - (a ^ (b << 3)) + (a | 7) - (b & 0xF0F0); }
CASE(c_signed) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    return (uint32_t)((x >> 3) + (x < y ? 1 : 0) * 5 + (x > 0 ? x : -x) - (y / 7) + (y % 9));
}
CASE(c_udiv) { return b ? a / b + a % b * 11 : a; }
CASE(c_sdiv) {
    int32_t x = (int32_t)a, y = (int32_t)b | 1;
    if (x == INT32_MIN && y == -1) y = 3;
    return (uint32_t)(x / y * 13 + x % y);
}
CASE(c_mul64) {
    uint64_t p = (uint64_t)a * b;
    int64_t s = (int64_t)(int32_t)a * (int32_t)b;
    return (uint32_t)(p >> 32) ^ (uint32_t)p ^ (uint32_t)(s >> 29);
}
CASE(c_shifts) {
    uint32_t n = b & 31;
    return (a << n) ^ (a >> n) ^ (uint32_t)((int32_t)a >> n) ^ ((a << n) | (a >> ((32 - n) & 31)));
}
CASE(c_bytes) {
    uint8_t x = (uint8_t)a, y = (uint8_t)b;
    uint16_t z = (uint16_t)(a >> 8);
    int8_t s = (int8_t)b;
    return (uint32_t)(uint8_t)(x + y) | (uint32_t)(uint16_t)(z * 3) << 8 | (uint32_t)(int32_t)s << 24 ^ (x > y);
}
CASE(c_switch) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 20; i++) {
        switch ((a + i) % 11) {
        case 0: r += 3; break;
        case 1: r ^= b; break;
        case 2: r = r * 5 + 1; break;
        case 3: r -= i; break;
        case 4: r += b >> 2; break;
        case 5: r |= 0x100; break;
        case 6: r = (r << 1) | (r >> 31); break;
        case 7: r += 77; break;
        case 8: r &= ~i; break;
        case 9: r ^= 0x5A5A; break;
        default: r += a; break;
        }
    }
    return r;
}
static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
CASE(c_recursion) { return fib(a % 20) + b; }
CASE(c_memory) {
    for (int i = 0; i < 64; i++) table[i] = a * (uint32_t)i + b;
    uint32_t s = 0;
    for (int i = 0; i < 64; i += 3) s += table[i] ^ table[63 - i];
    return s;
}
CASE(c_strings) {
    for (int i = 0; i < 200; i++) buf[i] = (char)(a + i * b);
    __builtin_memset(buf + 10, (int)b, 77);
    __builtin_memmove(buf + 30, buf + 5, 100);
    uint32_t h = 0;
    for (int i = 0; i < 200; i++) h = h * 31 + (uint8_t)buf[i];
    return h;
}
CASE(c_loop) {
    uint32_t r = 1;
    for (uint32_t i = 0; i < (a & 255); i++) {
        r = r * 1103515245u + 12345u;
        if (r & 0x400) r ^= b;
        else r += i;
    }
    return r;
}
/* Floating-point inputs stay moderate: near 2^31 the x87's 64-bit mantissa and a double's
 * 53 bits round differently, a known limit of modelling x87 with doubles. */
#define MOD(v) ((int32_t)(v) >> 12)
CASE(c_float) {
    float x = (float)MOD(a) / 1000.0f, y = (float)(b & 0xFFFF) / 7.0f;
    float r = x * y + x / (y + 1.0f) - (x > y ? x : y);
    return (uint32_t)(int32_t)(r * 64.0f);
}
CASE(c_double) {
    double x = (double)MOD(a) / 3.0, y = (double)(b >> 12) / 17.0 + 1.0;
    double r = x * x / y - y * 0.25 + (x < 0 ? -x : x);
    return (uint32_t)(int64_t)(r * 16.0);
}
CASE(c_fcompare) {
    double x = (double)(int32_t)a, y = (double)(int32_t)b;
    uint32_t r = 0;
    if (x < y) r |= 1;
    if (x <= y) r |= 2;
    if (x > y) r |= 4;
    if (x >= y) r |= 8;
    if (x == y) r |= 16;
    if (x != y) r |= 32;
    float f = (float)x;
    if (f < 0.5f) r |= 64;
    return r;
}
CASE(c_fconvert) {
    double d = (double)MOD(a) * 0.37 + (double)(b >> 12) * 1e-3;
    int32_t i = (int32_t)d;
    uint32_t u = (uint32_t)(d < 0 ? -d : d);
    int64_t l = (int64_t)(d * 1000.0);
    short s = (short)(d / 1000.0);  /* in range: out of range is undefined (x87 stores 0x8000) */
    return (uint32_t)i + u * 3 + (uint32_t)(l >> 4) + (uint32_t)(int32_t)s;
}
CASE(c_fmath) {
    double x = (double)(a & 0xFFFF) / 100.0 + 0.5;
    double r = __builtin_sqrt(x) * 100.0 + __builtin_fabs(-x) - __builtin_floor(x) * 2.0;
    return (uint32_t)(int32_t)(r * 10.0);
}
struct pt {
    int32_t x, y;
    uint8_t tag;
    float w;
};
static struct pt pts[16];
CASE(c_structs) {
    for (int i = 0; i < 16; i++) {
        pts[i].x = (int32_t)(a + (uint32_t)i);
        pts[i].y = (int32_t)(b - (uint32_t)i * 3);
        pts[i].tag = (uint8_t)(i * 7);
        pts[i].w = (float)i * 0.5f;
    }
    int32_t s = 0;
    for (int i = 0; i < 16; i++) s += pts[i].x * pts[15 - i].y + pts[i].tag + (int32_t)(pts[i].w * 4.0f);
    return (uint32_t)s;
}
CASE(c_minmax) {
    int32_t x = (int32_t)a, y = (int32_t)b;
    int32_t lo = x < y ? x : y, hi = x < y ? y : x;
    uint32_t ulo = a < b ? a : b;
    return (uint32_t)(lo * 3 + hi) ^ ulo ^ (uint32_t)(x != 0) ^ ((uint32_t)(y < 0) << 5);
}
CASE(c_bits) {
    uint32_t c = 0, x = a;
    while (x) {
        c += x & 1;
        x >>= 1;
    }
    uint32_t r = 0;
    for (int i = 0; i < 32; i++)
        if (b & (1u << i)) r = r * 3 + (uint32_t)i;
    return c * 1000 + r;
}
CASE(c_volatile) {
    sink = a;
    sink += b;
    return sink * 2;
}
CASE(c_xlat) {  /* a table lookup no compiler writes, but hand-written code (Kai's) does */
    for (int i = 0; i < 256; i++) buf[i] = (char)(i * 7 + b);
    uint32_t r = a;
    __asm__ volatile("xlatb" : "+a"(r) : "b"(buf) : "memory");
    return r;
}
