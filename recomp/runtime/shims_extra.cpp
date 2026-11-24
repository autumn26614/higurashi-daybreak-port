// What d3dx9_25.dll imports beyond the game's own imports: more of kernel32 and msvcrt,
// advapi32 (registry: nothing there), and the gdi32 calls of D3DX's font code (unused).
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <mutex>

#include "host.h"
#include "runtime.h"

using namespace rt;

namespace {
double darg(CPU *c, int slot) { return ldf64(c->r[4] + 4 + 4 * slot); }
uint32_t ret_double(CPU *c, double v) {
    FPUSH(v);
    return c->r[0];
}
}  // namespace

// ---- kernel32 ----
SHIM(kernel32, DisableThreadLibraryCalls, 4) { return 1; }
SHIM(kernel32, GetProcessHeap, 0) { return 0x50000; }
SHIM(kernel32, HeapAlloc, 12) { return galloc(arg(c, 2), (arg(c, 1) & 8) != 0); }
SHIM(kernel32, HeapFree, 12) {
    gfree(arg(c, 2));
    return 1;
}
SHIM(kernel32, InterlockedCompareExchange, 12) {
    uint32_t expected = arg(c, 2);
    __atomic_compare_exchange_n(reinterpret_cast<uint32_t *>(GM + arg(c, 0)), &expected, arg(c, 1), false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}
SHIM(kernel32, IsBadCodePtr, 4) { return 0; }
SHIM(kernel32, IsBadReadPtr, 8) { return arg(c, 0) == 0 && arg(c, 1) != 0; }
SHIM(kernel32, IsBadWritePtr, 8) { return arg(c, 0) == 0 && arg(c, 1) != 0; }
SHIM(kernel32, IsProcessorFeaturePresent, 4) { return 0; }
SHIM(kernel32, IsDBCSLeadByte, 4) {
    uint32_t b = arg(c, 0) & 0xFF;
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}
SHIM(kernel32, OutputDebugStringA, 4) {
    if (trace_imports) log("  debug: %s", gstr(arg(c, 0)));
    return 0;
}
SHIM(kernel32, SetUnhandledExceptionFilter, 4) { return 0; }
SHIM(kernel32, UnhandledExceptionFilter, 4) { return 0; }
SHIM(kernel32, TerminateProcess, 8) { fatal("TerminateProcess(%u)", arg(c, 1)); }
SHIM(kernel32, GlobalMemoryStatus, 4) {
    uint32_t o = arg(c, 0);
    memset(GM + o, 0, 32);
    st32(o, 32);
    st32(o + 8, 0x40000000);   // 1 GiB physical
    st32(o + 12, 0x30000000);  // available
    st32(o + 16, 0x80000000);
    st32(o + 20, 0x70000000);
    st32(o + 24, 0x7FFE0000);
    st32(o + 28, 0x60000000);
    return 0;
}
SHIM(kernel32, CreateMutexA, 12) { return handle_new(std::make_shared<KObject>()); }
SHIM(kernel32, ReleaseMutex, 4) { return 1; }
SHIM(kernel32, FindResourceA, 12) { return 0; }
SHIM(kernel32, FindResourceW, 12) { return 0; }
SHIM(kernel32, LoadResource, 8) { return 0; }
SHIM(kernel32, LockResource, 4) { return 0; }
SHIM(kernel32, SizeofResource, 8) { return 0; }
SHIM(kernel32, FreeResource, 4) { return 1; }
SHIM(kernel32, ExpandEnvironmentStringsA, 12) {
    std::string s = gstr(arg(c, 0));
    if (arg(c, 2) > s.size()) memcpy(GM + arg(c, 1), s.c_str(), s.size() + 1);
    return uint32_t(s.size() + 1);
}
SHIM(kernel32, GetFullPathNameA, 16) {
    std::string p = gstr(arg(c, 0));
    if (!(p.size() >= 2 && p[1] == ':')) p = guest_cwd() + p;
    if (arg(c, 1) <= p.size()) return uint32_t(p.size() + 1);
    memcpy(GM + arg(c, 2), p.c_str(), p.size() + 1);
    if (arg(c, 3)) {
        size_t k = p.find_last_of('\\');
        st32(arg(c, 3), arg(c, 2) + uint32_t(k == std::string::npos ? 0 : k + 1));
    }
    return uint32_t(p.size());
}
SHIM(kernel32, CompareStringA, 24) {
    std::string a(gstr(arg(c, 2)), int32_t(arg(c, 3)) < 0 ? strlen(gstr(arg(c, 2))) : arg(c, 3));
    std::string b(gstr(arg(c, 4)), int32_t(arg(c, 5)) < 0 ? strlen(gstr(arg(c, 4))) : arg(c, 5));
    int r = (arg(c, 1) & 1) ? strcasecmp(a.c_str(), b.c_str()) : strcmp(a.c_str(), b.c_str());
    return r < 0 ? 1 : r == 0 ? 2 : 3;
}
SHIM(kernel32, WideCharToMultiByte, 32) {
    uint32_t src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t len = int32_t(arg(c, 3));
    std::string out;
    for (uint32_t i = 0; len < 0 || i < uint32_t(len); i++) {
        uint32_t ch = ld16(src + 2 * i);
        out += ch < 0x80 ? char(ch) : '?';
        if (len < 0 && ch == 0) break;
    }
    if (!cap) return uint32_t(out.size());
    uint32_t n = std::min<uint32_t>(uint32_t(out.size()), cap);
    memcpy(GM + dst, out.data(), n);
    return n;
}
// Files d3dx would write (temp files, the W variants) are not supported.
SHIM(kernel32, CreateFileW, 28) { return 0xFFFFFFFF; }
SHIM(kernel32, DeleteFileA, 4) { return 0; }
SHIM(kernel32, DeleteFileW, 4) { return 0; }
SHIM(kernel32, MoveFileA, 8) { return 0; }
SHIM(kernel32, MoveFileW, 8) { return 0; }
SHIM(kernel32, SetEndOfFile, 4) { return 0; }
SHIM(kernel32, GetTempPathA, 8) { return 0; }
SHIM(kernel32, GetTempFileNameA, 16) { return 0; }
SHIM(kernel32, GetTempFileNameW, 16) { return 0; }
SHIM(kernel32, CreateFileMappingA, 24) { return 0; }
SHIM(kernel32, MapViewOfFile, 20) { return 0; }
SHIM(kernel32, UnmapViewOfFile, 4) { return 0; }
SHIM(kernel32, TerminateThread, 8) { return 0; }

// ---- advapi32: an empty registry ----
SHIM(advapi32, RegOpenKeyA, 12) { return 2; }
SHIM(advapi32, RegQueryValueExA, 24) { return 2; }
SHIM(advapi32, RegCloseKey, 4) { return 0; }

// ---- gdi32 calls of D3DX's font code (ID3DXFont; the game draws text itself) ----
SHIM(gdi32, CreateCompatibleDC, 4) { return 0x30002; }
SHIM(gdi32, DeleteDC, 4) { return 1; }
SHIM(gdi32, CreateDIBSection, 24) { return 0; }
SHIM(gdi32, CreateFontIndirectA, 4) { return 0x40100; }
SHIM(gdi32, CreateFontIndirectW, 4) { return 0x40100; }
SHIM(gdi32, GetCurrentObject, 8) { return 0x40000; }
SHIM(gdi32, GetObjectA, 12) { return 0; }
SHIM(gdi32, GetObjectW, 12) { return 0; }
SHIM(gdi32, SetBkColor, 8) { return 0; }
SHIM(gdi32, SetBkMode, 8) { return 1; }
SHIM(gdi32, SetMapMode, 8) { return 1; }
SHIM(gdi32, SetTextAlign, 8) { return 0; }
SHIM(gdi32, SetTextColor, 8) { return 0; }
SHIM(gdi32, MoveToEx, 16) { return 1; }
SHIM(gdi32, ExtTextOutA, 32) { return 1; }
SHIM(gdi32, ExtTextOutW, 32) { return 1; }
SHIM(gdi32, GetCharacterPlacementA, 24) { return 0; }
SHIM(gdi32, GetCharacterPlacementW, 24) { return 0; }
SHIM(gdi32, GetFontLanguageInfo, 4) { return 0; }
SHIM(gdi32, GetOutlineTextMetricsA, 12) { return 0; }
SHIM(gdi32, GetTextMetricsW, 8) { return 0; }
SHIM(gdi32, GetGlyphOutlineW, 28) { return 0xFFFFFFFF; }

// ---- msvcrt ----
SHIM_AS(msvcr71, op_new, "??2@YAPAXI@Z", kCdecl) { return galloc(arg(c, 0)); }
SHIM(msvcr71, _strdup, kCdecl) { return arg(c, 0) ? gstrdup(gstr(arg(c, 0))) : 0; }
SHIM(msvcr71, atof, kCdecl) { return ret_double(c, atof(gstr(arg(c, 0)))); }
SHIM(msvcr71, atol, kCdecl) { return uint32_t(atol(gstr(arg(c, 0)))); }
SHIM(msvcr71, _ultoa, kCdecl) {
    char buf[40];
    uint32_t v = arg(c, 0), radix = arg(c, 2);
    int i = 39;
    buf[i] = 0;
    do {
        uint32_t d = v % radix;
        buf[--i] = char(d < 10 ? '0' + d : 'a' + d - 10);
        v /= radix;
    } while (v);
    strcpy(gptr(arg(c, 1)), buf + i);
    return arg(c, 1);
}
SHIM(msvcr71, isalnum, kCdecl) { return isalnum(int(arg(c, 0) & 0xFF)) ? 1 : 0; }
SHIM(msvcr71, isalpha, kCdecl) { return isalpha(int(arg(c, 0) & 0xFF)) ? 1 : 0; }
SHIM(msvcr71, isdigit, kCdecl) { return isdigit(int(arg(c, 0) & 0xFF)) ? 1 : 0; }
SHIM(msvcr71, isspace, kCdecl) { return isspace(int(arg(c, 0) & 0xFF)) ? 1 : 0; }
SHIM(msvcr71, isxdigit, kCdecl) { return isxdigit(int(arg(c, 0) & 0xFF)) ? 1 : 0; }
SHIM(msvcr71, iswalpha, kCdecl) { return iswalpha(wint_t(arg(c, 0) & 0xFFFF)) ? 1 : 0; }
SHIM(msvcr71, iswdigit, kCdecl) { return iswdigit(wint_t(arg(c, 0) & 0xFFFF)) ? 1 : 0; }
SHIM(msvcr71, iswpunct, kCdecl) { return iswpunct(wint_t(arg(c, 0) & 0xFFFF)) ? 1 : 0; }
SHIM(msvcr71, iswspace, kCdecl) { return iswspace(wint_t(arg(c, 0) & 0xFFFF)) ? 1 : 0; }
SHIM(msvcr71, tolower, kCdecl) { return uint32_t(tolower(int(arg(c, 0)))); }
SHIM(msvcr71, rand, kCdecl) {
    static uint32_t seed = 1;  // the MSVC generator
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    seed = seed * 214013 + 2531011;
    return (seed >> 16) & 0x7FFF;
}
SHIM(msvcr71, srand, kCdecl) { return 0; }
SHIM(msvcr71, setlocale, kCdecl) {
    static uint32_t c_locale = gstrdup("C");
    return c_locale;
}
SHIM(msvcr71, _finite, kCdecl) { return std::isfinite(darg(c, 0)) ? 1 : 0; }
SHIM(msvcr71, _isnan, kCdecl) { return std::isnan(darg(c, 0)) ? 1 : 0; }
SHIM(msvcr71, _fpclass, kCdecl) {
    double x = darg(c, 0);
    bool neg = std::signbit(x);
    switch (std::fpclassify(x)) {
    case FP_NAN: return 2;
    case FP_INFINITE: return neg ? 4 : 0x200;
    case FP_ZERO: return neg ? 0x20 : 0x40;
    case FP_SUBNORMAL: return neg ? 0x10 : 0x80;
    default: return neg ? 8 : 0x100;
    }
}
SHIM(msvcr71, modf, kCdecl) {
    double ip;
    double fr = modf(darg(c, 0), &ip);
    stf64(arg(c, 2), ip);
    return ret_double(c, fr);
}
SHIM(msvcr71, _CIsqrt, kCdecl) {
    ST(0) = sqrt(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CIasin, kCdecl) {
    ST(0) = asin(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CIexp, kCdecl) {
    ST(0) = exp(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CIsinh, kCdecl) {
    ST(0) = sinh(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CIcosh, kCdecl) {
    ST(0) = cosh(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CItanh, kCdecl) {
    ST(0) = tanh(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _CIfmod, kCdecl) {
    double y = ST(0), x = ST(1);
    FPOP();
    ST(0) = fmod(x, y);
    return c->r[0];
}
SHIM(msvcr71, fwrite, kCdecl) { return arg(c, 2); }  // d3dx only writes debug and temp files
SHIM(msvcr71, _wfopen, kCdecl) { return 0; }
SHIM(msvcr71, _tempnam, kCdecl) { return 0; }
SHIM(msvcr71, _strdate, kCdecl) {
    strcpy(gptr(arg(c, 0)), "01/01/06");
    return arg(c, 0);
}
SHIM(msvcr71, _strtime, kCdecl) {
    strcpy(gptr(arg(c, 0)), "00:00:00");
    return arg(c, 0);
}
SHIM(msvcr71, wcstombs, kCdecl) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), n = arg(c, 2);
    uint32_t i = 0;
    for (; ; i++) {
        uint32_t ch = ld16(src + 2 * i);
        if (dst && i < n) st8(dst + i, ch < 0x80 ? ch : '?');
        if (!ch || (dst && i + 1 >= n)) break;
    }
    return i;
}
// setjmp/longjmp: only used on error paths of the effect and X-file parsers.
SHIM(msvcr71, _setjmp3, kCdecl) { return 0; }
SHIM(msvcr71, longjmp, kCdecl) { fatal("longjmp (an error path in d3dx9)"); }

// sscanf over guest arguments (the conversions D3DX uses).
SHIM(msvcr71, sscanf, kCdecl) {
    const char *s = gstr(arg(c, 0));
    const char *f = gstr(arg(c, 1));
    uint32_t ap = c->r[4] + 12;
    const char *start = s;
    int assigned = 0;
    while (*f) {
        if (isspace(static_cast<unsigned char>(*f))) {
            while (isspace(static_cast<unsigned char>(*s))) s++;
            f++;
            continue;
        }
        if (*f != '%') {
            if (*s != *f) break;
            s++;
            f++;
            continue;
        }
        f++;
        bool skip = false;
        if (*f == '*') {
            skip = true;
            f++;
        }
        int width = 0;
        while (isdigit(static_cast<unsigned char>(*f))) width = width * 10 + (*f++ - '0');
        int len = 0;
        if (*f == 'l') { len = 1; f++; }
        else if (*f == 'h') { len = 2; f++; }
        char conv = *f++;
        if (conv == '%') {
            if (*s != '%') break;
            s++;
            continue;
        }
        if (conv == 'n') {
            if (!skip) st32(ld32(ap), uint32_t(s - start));
            if (!skip) ap += 4;
            continue;
        }
        if (conv != 'c')
            while (isspace(static_cast<unsigned char>(*s))) s++;
        if (!*s) break;
        std::string tok;
        if (conv == 'c') {
            int n = width ? width : 1;
            tok.assign(s, std::min<size_t>(size_t(n), strlen(s)));
        } else if (conv == 's') {
            while (*s && !isspace(static_cast<unsigned char>(*s)) && (!width || int(tok.size()) < width)) tok += *s++;
            s -= tok.size();
        } else {
            size_t n = strlen(s);
            if (width) n = std::min<size_t>(n, size_t(width));
            tok.assign(s, n);
        }
        char *end = nullptr;
        uint32_t dst = skip ? 0 : ld32(ap);
        size_t used = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            long long v = strtoll(tok.c_str(), &end, base);
            used = size_t(end - tok.c_str());
            if (!used) goto done;
            if (dst) len == 2 ? st16(dst, uint32_t(v)) : st32(dst, uint32_t(v));
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': {
            double v = strtod(tok.c_str(), &end);
            used = size_t(end - tok.c_str());
            if (!used) goto done;
            if (dst) len == 1 ? stf64(dst, v) : stf32(dst, v);
            break;
        }
        case 's':
            used = tok.size();
            if (dst) memcpy(GM + dst, tok.c_str(), tok.size() + 1);
            break;
        case 'c':
            used = tok.size();
            if (dst) memcpy(GM + dst, tok.data(), tok.size());
            break;
        default:
            goto done;
        }
        s += used;
        if (!skip) {
            ap += 4;
            assigned++;
        }
    }
done:
    return uint32_t(assigned);
}
