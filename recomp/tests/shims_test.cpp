// Calls the C runtime and kernel32 shims the way the game does (through their import thunks,
// arguments on the guest stack) and checks the results against the host C library.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"

using namespace rt;

namespace {
CPU *cpu;
int failures = 0, checks = 0;

uint32_t fn(const char *dll, const char *name) {
    uint32_t a = resolve_import(dll, name);
    if (!a) fatal("no shim %s!%s", dll, name);
    return a;
}
uint32_t gs(const std::string &s) { return gstrdup(s); }
uint32_t lo(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return uint32_t(u);
}
uint32_t hi(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return uint32_t(u >> 32);
}
void expect(bool ok, const char *fmt, ...) {
    checks++;
    if (ok) return;
    failures++;
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

// sprintf through the shim with up to six 32-bit argument words.
std::string guest_sprintf(const char *f, std::initializer_list<uint32_t> words) {
    uint32_t buf = galloc(512, true);
    std::vector<uint32_t> a{buf, gs(f)};
    a.insert(a.end(), words.begin(), words.end());
    while (a.size() < 8) a.push_back(0);
    call_guest(cpu, fn("msvcr71.dll", "sprintf"), {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]});
    return gstr(buf);
}

void test_sprintf() {
    struct {
        const char *f;
        std::initializer_list<uint32_t> w;
        std::string want;
    } cases[] = {
        {"%d %i %u", {uint32_t(-42), 7, 3000000000u}, "-42 7 3000000000"},
        {"%5d|%-5d|%05d", {42, 42, 42}, "   42|42   |00042"},
        {"%x %X %08x %#x", {0xBEEF, 0xBEEF, 0x12, 255}, "beef BEEF 00000012 0xff"},
        {"%s and %.2s", {0, 0}, ""},  // replaced below (needs guest strings)
        {"%c%c%c", {'a', 'b', 'c'}, "abc"},
        {"%%%d%%", {5}, "%5%"},
        {"%hd %hu", {0x18000, 0x18000}, "-32768 32768"},
        {"%ld", {uint32_t(-7)}, "-7"},
        {"%I64d", {0xFFFFFFFF, 0xFFFFFFFF}, "-1"},
        {"%*d|%.*s", {6, 9, 2, 0}, ""},  // replaced below
    };
    for (auto &c : cases) {
        if (std::string(c.f) == "%s and %.2s") {
            std::string got = guest_sprintf(c.f, {gs("hello"), gs("world")});
            expect(got == "hello and wo", "sprintf %%s: '%s'", got.c_str());
            continue;
        }
        if (std::string(c.f) == "%*d|%.*s") {
            std::string got = guest_sprintf(c.f, {6, 9, 2, gs("abc")});
            expect(got == "     9|ab", "sprintf *: '%s'", got.c_str());
            continue;
        }
        std::string got = guest_sprintf(c.f, c.w);
        expect(got == c.want, "sprintf \"%s\": '%s' want '%s'", c.f, got.c_str(), c.want.c_str());
    }
    double d = 3.14159;
    std::string got = guest_sprintf("%.2f %e %g", {lo(d), hi(d), lo(d), hi(d), lo(d), hi(d)});
    char want[64];
    snprintf(want, sizeof want, "%.2f %e %g", d, d, d);
    expect(got == want, "sprintf floats: '%s' want '%s'", got.c_str(), want);
    got = guest_sprintf("%5.1f|%-8.3f|", {lo(-2.25), hi(-2.25), lo(1.0), hi(1.0)});
    expect(got == " -2.2|1.000   |" || got == " -2.3|1.000   |", "sprintf widths: '%s'", got.c_str());
}

void test_vsnprintf() {
    uint32_t buf = galloc(16, true), args = galloc(8, true);
    st32(args, 12345);
    uint32_t r = call_guest(cpu, fn("msvcr71.dll", "_vsnprintf"), {buf, 5, gs("%d"), args});
    expect(r == 5 && memcmp(GM + buf, "12345", 5) == 0, "_vsnprintf exact fit: %d", int(r));
    r = call_guest(cpu, fn("msvcr71.dll", "_vsnprintf"), {buf, 3, gs("%d"), args});
    expect(int32_t(r) == -1 && memcmp(GM + buf, "123", 3) == 0, "_vsnprintf truncated: %d", int(r));
    r = call_guest(cpu, fn("msvcr71.dll", "_vsnprintf"), {buf, 10, gs("%d"), args});
    expect(r == 5 && std::string(gstr(buf)) == "12345", "_vsnprintf room: %d", int(r));
}

void test_sscanf() {
    uint32_t i = galloc(4, true), f = galloc(4, true), d = galloc(8, true), s = galloc(32, true), n = galloc(4, true);
    uint32_t r = call_guest(cpu, fn("msvcrt.dll", "sscanf"),
                            {gs("  42 3.5 -1e2 word rest"), gs("%d %f %lf %s%n"), i, f, d, s, n});
    float fv;
    memcpy(&fv, GM + f, 4);
    expect(r == 4 && int32_t(ld32(i)) == 42 && fv == 3.5f && ldf64(d) == -100.0 && std::string(gstr(s)) == "word" &&
               ld32(n) == 18,
           "sscanf: %d %d %g %g '%s' n=%u", int(r), int32_t(ld32(i)), double(fv), ldf64(d), gstr(s), ld32(n));
    r = call_guest(cpu, fn("msvcrt.dll", "sscanf"), {gs("x=0x1F,y=-3"), gs("x=%x,y=%d"), i, n});
    expect(r == 2 && ld32(i) == 0x1F && int32_t(ld32(n)) == -3, "sscanf hex: %d %x %d", int(r), ld32(i), int32_t(ld32(n)));
    r = call_guest(cpu, fn("msvcrt.dll", "sscanf"), {gs("abc"), gs("%d"), i});
    expect(r == 0, "sscanf no match: %d", int(r));
}

void test_qsort() {
    // A guest comparator would need translated code; use the C runtime's own: none exists, so
    // check the algorithm through a host comparator thunk.
    uint32_t cmp = make_thunk("test cmp", kCdecl, [](CPU *c) {
        int32_t a = int32_t(ld32(arg(c, 0))), b = int32_t(ld32(arg(c, 1)));
        return uint32_t(a < b ? -1 : a > b ? 1 : 0);
    });
    for (uint32_t n : {0u, 1u, 2u, 7u, 8u, 9u, 50u, 1000u}) {
        uint32_t arr = galloc(4 * n + 4, true);
        uint32_t seed = 12345;
        for (uint32_t k = 0; k < n; k++) {
            seed = seed * 1103515245u + 12345u;
            st32(arr + 4 * k, (seed >> 8) % 100);
        }
        call_guest(cpu, fn("msvcr71.dll", "qsort"), {arr, n, 4, cmp});
        bool sorted = true;
        for (uint32_t k = 1; k < n; k++) sorted = sorted && int32_t(ld32(arr + 4 * k - 4)) <= int32_t(ld32(arr + 4 * k));
        expect(sorted, "qsort %u elements", n);
    }
}

void test_strings() {
    uint32_t drive = galloc(8, true), dir = galloc(64, true), name = galloc(64, true), ext = galloc(16, true);
    call_guest(cpu, fn("msvcr71.dll", "_splitpath"), {gs("C:\\Game\\data\\file.name.dat"), drive, dir, name, ext});
    expect(std::string(gstr(drive)) == "C:" && std::string(gstr(dir)) == "\\Game\\data\\" &&
               std::string(gstr(name)) == "file.name" && std::string(gstr(ext)) == ".dat",
           "_splitpath: '%s' '%s' '%s' '%s'", gstr(drive), gstr(dir), gstr(name), gstr(ext));
    uint32_t s = gs("a/b\\c.d");
    uint32_t r = call_guest(cpu, fn("msvcr71.dll", "strrchr"), {s, '\\'});
    expect(r == s + 3, "strrchr");
    r = call_guest(cpu, fn("msvcr71.dll", "strchr"), {s, 'z'});
    expect(r == 0, "strchr missing");
    expect(int32_t(call_guest(cpu, fn("msvcr71.dll", "_stricmp"), {gs("Hello"), gs("hELLO")})) == 0, "_stricmp equal");
    expect(int32_t(call_guest(cpu, fn("msvcr71.dll", "_strnicmp"), {gs("abcX"), gs("ABCy"), 3})) == 0, "_strnicmp");
    expect(int32_t(call_guest(cpu, fn("msvcr71.dll", "strcmp"), {gs("a"), gs("b")})) < 0, "strcmp");
    expect(call_guest(cpu, fn("msvcr71.dll", "atoi"), {gs("  -123xyz")}) == uint32_t(-123), "atoi");
    expect(call_guest(cpu, fn("kernel32.dll", "lstrlenA"), {gs("12345")}) == 5, "lstrlenA");
    expect(call_guest(cpu, fn("kernel32.dll", "lstrlenA"), {0}) == 0, "lstrlenA(NULL)");
    // CompareStringA: NORM_IGNORECASE
    expect(call_guest(cpu, fn("kernel32.dll", "CompareStringA"), {0x411, 1, gs("abc"), uint32_t(-1), gs("ABC"), uint32_t(-1)}) == 2,
           "CompareStringA equal");
    expect(call_guest(cpu, fn("kernel32.dll", "CompareStringA"), {0x411, 0, gs("ab"), uint32_t(-1), gs("abc"), uint32_t(-1)}) == 1,
           "CompareStringA less");
}

void test_codepage() {
    // "日本" in Shift-JIS: 93 FA 96 7B -> U+65E5 U+672C
    uint32_t src = galloc(8, true), dst = galloc(16, true);
    const uint8_t sj[] = {0x93, 0xFA, 0x96, 0x7B, 0};
    memcpy(GM + src, sj, 5);
    uint32_t n = call_guest(cpu, fn("kernel32.dll", "MultiByteToWideChar"), {932, 0, src, uint32_t(-1), dst, 8});
    expect(n == 3 && ld16(dst) == 0x65E5 && ld16(dst + 2) == 0x672C && ld16(dst + 4) == 0,
           "MultiByteToWideChar: %u %04x %04x", n, ld16(dst), ld16(dst + 2));
    uint32_t lead = call_guest(cpu, fn("msvcr71.dll", "_ismbblead"), {0x93});
    expect(lead != 0 && call_guest(cpu, fn("msvcr71.dll", "_ismbblead"), {0x41}) == 0, "_ismbblead");
}

void test_math() {
    // _ftol: pops ST(0), returns edx:eax.
    cpu->top = 0;
    {
        CPU *c = cpu;
        FPUSH(-2.75);
    }
    uint32_t r = call_guest(cpu, fn("msvcr71.dll", "_ftol"), {});
    expect(int32_t(r) == -2 && cpu->r[2] == 0xFFFFFFFF, "_ftol(-2.75) = %d, edx %08x", int32_t(r), cpu->r[2]);
    // pow(2, 10) returns in ST(0)
    call_guest(cpu, fn("msvcr71.dll", "pow"), {lo(2.0), hi(2.0), lo(10.0), hi(10.0)});
    double v = cpu->st[cpu->top & 7];
    cpu->top = (cpu->top + 1) & 7;
    expect(v == 1024.0, "pow: %g", v);
    call_guest(cpu, fn("msvcr71.dll", "atan2"), {lo(1.0), hi(1.0), lo(1.0), hi(1.0)});
    v = cpu->st[cpu->top & 7];
    cpu->top = (cpu->top + 1) & 7;
    expect(std::fabs(v - M_PI / 4) < 1e-12, "atan2: %g", v);
    uint32_t e = galloc(4, true);
    call_guest(cpu, fn("msvcr71.dll", "frexp"), {lo(48.0), hi(48.0), e});
    v = cpu->st[cpu->top & 7];
    cpu->top = (cpu->top + 1) & 7;
    expect(v == 0.75 && ld32(e) == 6, "frexp: %g %u", v, ld32(e));
    // _controlfp: precision and rounding round-trip
    uint32_t old = call_guest(cpu, fn("msvcr71.dll", "_controlfp"), {0, 0});
    call_guest(cpu, fn("msvcr71.dll", "_controlfp"), {0x20000 | 0x300, 0x30300});
    expect(((cpu->fcw >> 8) & 3) == 0 && ((cpu->fcw >> 10) & 3) == 3, "_controlfp set: fcw %04x", cpu->fcw);
    uint32_t now = call_guest(cpu, fn("msvcr71.dll", "_controlfp"), {0, 0});
    expect((now & 0x30300) == 0x20300, "_controlfp read: %08x", now);
    call_guest(cpu, fn("msvcr71.dll", "_controlfp"), {old, 0x30300});
}

void test_heap() {
    uint32_t a = call_guest(cpu, fn("msvcr71.dll", "malloc"), {100});
    uint32_t b = call_guest(cpu, fn("msvcr71.dll", "calloc"), {10, 10});
    bool zero = true;
    for (int k = 0; k < 100; k++) zero = zero && GM[b + uint32_t(k)] == 0;
    expect(a && b && a != b && (a & 15) == 0 && zero, "malloc/calloc");
    memset(GM + a, 0x5A, 100);
    uint32_t c = call_guest(cpu, fn("msvcr71.dll", "realloc"), {a, 5000});
    expect(c && GM[c] == 0x5A && GM[c + 99] == 0x5A, "realloc keeps contents");
    call_guest(cpu, fn("msvcr71.dll", "free"), {c});
    call_guest(cpu, fn("msvcr71.dll", "free"), {0});
    uint32_t d = call_guest(cpu, fn("msvcr71.dll", "malloc"), {5000});
    expect(d == c, "freed block reused");
}
}  // namespace

int main() {
    memory_init();
    cpu = new_cpu(kMainStack);
    test_sprintf();
    test_vsnprintf();
    test_sscanf();
    test_qsort();
    test_strings();
    test_codepage();
    test_math();
    test_heap();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
