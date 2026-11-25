// What Higurashi Daybreak Kai's DaybreakDX.exe imports beyond the original game: it links
// the C runtime statically, so the runtime's own needs come straight from kernel32 (heaps,
// thread-local storage, environment, locale and code-page queries, standard handles).
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unistd.h>

#include "host.h"
#include "runtime.h"

using namespace rt;

namespace {
thread_local uint32_t last_error_kai = 0;
}

// ---- heaps: one guest heap behind every handle ----
SHIM(kernel32, HeapCreate, 12) { return 0x50000; }
SHIM(kernel32, HeapDestroy, 4) { return 1; }
SHIM(kernel32, HeapReAlloc, 16) {
    uint32_t p = arg(c, 2), n = arg(c, 3);
    if (!p) return galloc(n, (arg(c, 1) & 8) != 0);
    uint32_t old = gsize(p);
    if (arg(c, 1) & 0x10) return n <= old ? p : 0;  // HEAP_REALLOC_IN_PLACE_ONLY
    uint32_t q = grealloc(p, n);
    if ((arg(c, 1) & 8) && n > old) memset(GM + q + old, 0, n - old);
    return q;
}
SHIM(kernel32, HeapSize, 12) { return gsize(arg(c, 2)); }

// ---- thread-local storage: the TEB's TlsSlots (fs:[0xE10], 64 slots) ----
namespace {
std::mutex tls_mutex;
uint64_t tls_used = 0;
}  // namespace
SHIM(kernel32, TlsAlloc, 0) {
    std::lock_guard<std::mutex> lock(tls_mutex);
    for (uint32_t i = 0; i < 64; i++)
        if (!(tls_used & (uint64_t(1) << i))) {
            tls_used |= uint64_t(1) << i;
            return i;
        }
    return 0xFFFFFFFF;  // TLS_OUT_OF_INDEXES
}
SHIM(kernel32, TlsFree, 4) {
    std::lock_guard<std::mutex> lock(tls_mutex);
    if (arg(c, 0) < 64) tls_used &= ~(uint64_t(1) << arg(c, 0));
    return 1;
}
SHIM(kernel32, TlsGetValue, 4) { return arg(c, 0) < 64 ? ld32(c->fs_base + 0xE10 + 4 * arg(c, 0)) : 0; }
SHIM(kernel32, TlsSetValue, 8) {
    if (arg(c, 0) >= 64) return 0;
    st32(c->fs_base + 0xE10 + 4 * arg(c, 0), arg(c, 1));
    return 1;
}

// ---- process environment ----
SHIM(kernel32, GetCommandLineA, 0) {
    static uint32_t s = gstrdup("\"C:\\Daybreak\\DaybreakDX.exe\"");
    return s;
}
SHIM(kernel32, GetEnvironmentStrings, 0) {
    static uint32_t s = [] {
        const char env[] = "PATH=C:\\Windows\0\0";
        uint32_t a = galloc(sizeof env);
        memcpy(GM + a, env, sizeof env);
        return a;
    }();
    return s;
}
SHIM(kernel32, GetEnvironmentStringsW, 0) {
    static uint32_t s = [] {
        const char *env = "PATH=C:\\Windows";
        size_t n = strlen(env);
        uint32_t a = galloc(uint32_t(2 * (n + 2)), true);
        for (size_t i = 0; i < n; i++) st16(a + 2 * uint32_t(i), uint8_t(env[i]));
        return a;
    }();
    return s;
}
SHIM(kernel32, FreeEnvironmentStringsA, 4) { return 1; }
SHIM(kernel32, FreeEnvironmentStringsW, 4) { return 1; }
SHIM(kernel32, SetLastError, 4) {
    last_error_kai = arg(c, 0);
    return 0;
}

// ---- standard handles (no console) ----
SHIM(kernel32, GetStdHandle, 4) {
    uint32_t which = arg(c, 0);  // -10 input, -11 output, -12 error
    return which >= 0xFFFFFFF4 ? 0x7F000000 + (0xFFFFFFF6 - which) : 0xFFFFFFFF;
}
SHIM(kernel32, SetStdHandle, 8) { return 1; }
SHIM(kernel32, GetFileType, 4) {
    uint32_t h = arg(c, 0);
    if (h >= 0x7F000000 && h < 0x7F000003) return 2;  // FILE_TYPE_CHAR
    return handle_get(h) ? 1 : 0;                      // FILE_TYPE_DISK / UNKNOWN
}
SHIM(kernel32, SetHandleCount, 4) { return arg(c, 0); }
SHIM(kernel32, FlushFileBuffers, 4) { return 1; }

// ---- code pages and locale: Japanese (932) ----
SHIM(kernel32, GetACP, 0) { return 932; }
SHIM(kernel32, GetOEMCP, 0) { return 932; }
SHIM(kernel32, GetCPInfo, 8) {
    uint32_t p = arg(c, 1);
    memset(GM + p, 0, 20);
    if (arg(c, 0) == 932 || arg(c, 0) == 0) {
        st32(p, 2);  // MaxCharSize
        st8(p + 4, '?');
        const uint8_t lead[] = {0x81, 0x9F, 0xE0, 0xFC};  // lead byte ranges
        memcpy(GM + p + 6, lead, sizeof lead);
    } else {
        st32(p, 1);
        st8(p + 4, '?');
    }
    return 1;
}
SHIM(kernel32, GetLocaleInfoA, 16) {
    uint32_t type = arg(c, 1) & 0xFFFF, out = arg(c, 2), n = arg(c, 3);
    std::string v;
    switch (type) {
    case 0x1004: v = "932"; break;   // LOCALE_IDEFAULTANSICODEPAGE
    case 0x000B: v = "932"; break;   // LOCALE_IDEFAULTCODEPAGE
    case 0x0059: v = "ja"; break;    // LOCALE_SISO639LANGNAME
    case 0x005A: v = "JP"; break;    // LOCALE_SISO3166CTRYNAME
    case 0x000E: v = "."; break;     // LOCALE_SDECIMAL
    case 0x000F: v = ","; break;     // LOCALE_STHOUSAND
    default: v = ""; break;
    }
    if (!n) return uint32_t(v.size() + 1);
    if (n <= v.size()) return 0;
    memcpy(GM + out, v.c_str(), v.size() + 1);
    return uint32_t(v.size() + 1);
}
namespace {
uint16_t char_type(uint32_t ch) {  // CT_CTYPE1 bits
    uint16_t t = 0;
    if (ch < 0x80) {
        if (isupper(int(ch))) t |= 0x001;
        if (islower(int(ch))) t |= 0x002;
        if (isdigit(int(ch))) t |= 0x004;
        if (isspace(int(ch))) t |= 0x008;
        if (ispunct(int(ch))) t |= 0x010;
        if (iscntrl(int(ch))) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040;
        if (isxdigit(int(ch))) t |= 0x080;
        if (isalpha(int(ch))) t |= 0x100;
    } else {
        t |= 0x100;  // treat other characters as letters
    }
    return t;
}
}  // namespace
SHIM(kernel32, GetStringTypeA, 20) {
    uint32_t src = arg(c, 2), out = arg(c, 4);
    int32_t n = int32_t(arg(c, 3));
    if (n < 0) n = int32_t(strlen(gstr(src)));
    for (int32_t i = 0; i < n; i++) st16(out + 2 * uint32_t(i), char_type(ld8(src + uint32_t(i))));
    return 1;
}
SHIM(kernel32, GetStringTypeW, 16) {
    uint32_t src = arg(c, 1), out = arg(c, 3);
    int32_t n = int32_t(arg(c, 2));
    if (n < 0) {
        n = 0;
        while (ld16(src + 2 * uint32_t(n))) n++;
    }
    for (int32_t i = 0; i < n; i++) st16(out + 2 * uint32_t(i), char_type(ld16(src + 2 * uint32_t(i))));
    return 1;
}
SHIM(kernel32, LCMapStringA, 24) {
    uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = int32_t(arg(c, 3));
    if (n < 0) n = int32_t(strlen(gstr(src))) + 1;
    if (!cap) return uint32_t(n);
    if (uint32_t(n) > cap) return 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t ch = ld8(src + uint32_t(i));
        if (flags & 0x100) ch = uint32_t(tolower(int(ch)));  // LCMAP_LOWERCASE
        if (flags & 0x200) ch = uint32_t(toupper(int(ch)));  // LCMAP_UPPERCASE
        st8(dst + uint32_t(i), ch);
    }
    return uint32_t(n);
}
SHIM(kernel32, LCMapStringW, 24) {
    uint32_t flags = arg(c, 1), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t n = int32_t(arg(c, 3));
    if (n < 0) {
        n = 0;
        while (ld16(src + 2 * uint32_t(n))) n++;
        n++;
    }
    if (!cap) return uint32_t(n);
    if (uint32_t(n) > cap) return 0;
    for (int32_t i = 0; i < n; i++) {
        uint32_t ch = ld16(src + 2 * uint32_t(i));
        if (ch < 0x80 && (flags & 0x100)) ch = uint32_t(tolower(int(ch)));
        if (ch < 0x80 && (flags & 0x200)) ch = uint32_t(toupper(int(ch)));
        st16(dst + 2 * uint32_t(i), ch);
    }
    return uint32_t(n);
}
SHIM(kernel32, MulDiv, 12) {
    int64_t p = int64_t(int32_t(arg(c, 0))) * int32_t(arg(c, 1));
    int32_t d = int32_t(arg(c, 2));
    if (!d) return 0xFFFFFFFF;
    int64_t q = p / d, r = p % d;  // rounded to nearest, halves away from zero
    if (2 * (r < 0 ? -r : r) >= (d < 0 ? -int64_t(d) : int64_t(d))) q += (p < 0) != (d < 0) ? -1 : 1;
    if (q > INT32_MAX || q < INT32_MIN) return 0xFFFFFFFF;
    return uint32_t(int32_t(q));
}

// ---- memory ----
SHIM(kernel32, VirtualProtect, 16) {
    if (arg(c, 3)) st32(arg(c, 3), 0x04);  // PAGE_READWRITE
    return 1;
}
SHIM(kernel32, VirtualQuery, 12) {
    uint32_t a = arg(c, 0), out = arg(c, 1);
    memset(GM + out, 0, 28);
    st32(out, a & ~0xFFFu);         // BaseAddress
    st32(out + 4, a & ~0xFFFFu);    // AllocationBase
    st32(out + 8, 0x04);
    st32(out + 12, 0x10000);        // RegionSize
    st32(out + 16, 0x1000);         // MEM_COMMIT
    st32(out + 20, 0x04);
    st32(out + 24, 0x20000);        // MEM_PRIVATE
    return 28;
}

// ---- exceptions: the game does not throw in normal play ----
SHIM(kernel32, RaiseException, 16) { fatal("RaiseException(%08x)", arg(c, 0)); }
SHIM(kernel32, RtlUnwind, 16) { fatal("RtlUnwind (structured exception unwinding)"); }

// ---- user32, imm32 ----
SHIM(user32, wsprintfA, kCdecl) {
    std::string s;
    format_guest(s, gstr(arg(c, 1)), c->r[4] + 12);
    if (s.size() > 1023) s.resize(1023);  // wsprintf's limit
    memcpy(GM + arg(c, 0), s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}
SHIM(imm32, ImmSetOpenStatus, 8) { return 1; }
