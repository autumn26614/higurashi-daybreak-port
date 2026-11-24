// msvcr71 / msvcp71: the C and C++ runtime functions daybreak.exe imports, on guest memory.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <map>
#include <mutex>
#include <vector>

#include "host.h"
#include "runtime.h"

using namespace rt;

namespace {
double darg(CPU *c, int slot) { return ldf64(c->r[4] + 4 + 4 * slot); }  // double at 32-bit arg slot
uint32_t ret_double(CPU *c, double v) {
    FPUSH(v);
    return c->r[0];
}

// FILE*: a guest block whose first word is our index.
std::mutex file_mutex;
std::map<uint32_t, FILE *> files;

FILE *host_file(uint32_t f) {
    std::lock_guard<std::mutex> lock(file_mutex);
    auto it = files.find(f);
    return it == files.end() ? nullptr : it->second;
}

std::vector<uint32_t> &atexit_fns() {
    static std::vector<uint32_t> v;
    return v;
}

uint32_t errno_block() {
    thread_local uint32_t a = 0;
    if (!a) a = galloc(4, true);
    return a;
}
}  // namespace

// ---- process startup ----
static uint32_t g_acmdln, g_adjust_fdiv, g_fmode, g_commode;
static DataReg reg_acmdln("msvcr71", "_acmdln", [] {
    if (!g_acmdln) {
        g_acmdln = galloc(4);
        st32(g_acmdln, gstrdup("C:\\Daybreak\\daybreak.exe"));
    }
    return g_acmdln;
});
static DataReg reg_adjust_fdiv("msvcr71", "_adjust_fdiv", [] {
    if (!g_adjust_fdiv) g_adjust_fdiv = galloc(4, true);
    return g_adjust_fdiv;
});

SHIM(msvcr71, __set_app_type, kCdecl) { return 0; }
SHIM(msvcr71, __setusermatherr, kCdecl) { return 0; }
SHIM(msvcr71, __p__fmode, kCdecl) {
    if (!g_fmode) g_fmode = galloc(4, true);
    return g_fmode;
}
SHIM(msvcr71, __p__commode, kCdecl) {
    if (!g_commode) g_commode = galloc(4, true);
    return g_commode;
}
SHIM(msvcr71, _controlfp, kCdecl) {
    // Abstract control bits: _MCW_RC 0x300 (near 0, down 0x100, up 0x200, chop 0x300).
    uint32_t nw = arg(c, 0), mask = arg(c, 1);
    uint32_t rc = (c->fcw >> 10) & 3;
    uint32_t cur = (rc == 1 ? 0x100 : rc == 2 ? 0x200 : rc == 3 ? 0x300 : 0) | 0x8001F;
    if (mask & 0x300) {
        uint32_t want = nw & 0x300;
        uint32_t bits = want == 0x100 ? 1 : want == 0x200 ? 2 : want == 0x300 ? 3 : 0;
        c->fcw = uint16_t((c->fcw & ~0x0C00) | (bits << 10));
        cur = (cur & ~0x300u) | want;
    }
    return cur;
}
SHIM(msvcr71, __getmainargs, kCdecl) {
    static uint32_t argv = 0, envp = 0;
    if (!argv) {
        argv = galloc(8, true);
        st32(argv, gstrdup("C:\\Daybreak\\daybreak.exe"));
        envp = galloc(4, true);
    }
    st32(arg(c, 0), 1);
    st32(arg(c, 1), argv);
    st32(arg(c, 2), envp);
    return 0;
}
SHIM(msvcr71, _initterm, kCdecl) {
    for (uint32_t p = arg(c, 0); p < arg(c, 1); p += 4)
        if (uint32_t fn = ld32(p)) call_guest(c, fn, {});
    return 0;
}
SHIM(msvcr71, _onexit, kCdecl) {
    atexit_fns().push_back(arg(c, 0));
    return arg(c, 0);
}
SHIM(msvcr71, __dllonexit, kCdecl) {
    atexit_fns().push_back(arg(c, 0));
    return arg(c, 0);
}
static void run_atexit(CPU *c) {
    auto fns = atexit_fns();
    atexit_fns().clear();
    for (auto it = fns.rbegin(); it != fns.rend(); ++it) call_guest(c, *it, {});
}
SHIM(msvcr71, exit, kCdecl) {
    run_atexit(c);
    log("recomp: exit(%d)", int(arg(c, 0)));
    fflush(nullptr);
    _exit(int(arg(c, 0)));
}
SHIM(msvcr71, _exit, kCdecl) {
    fflush(nullptr);
    _exit(int(arg(c, 0)));
}
SHIM(msvcr71, _cexit, kCdecl) {
    run_atexit(c);
    return 0;
}
SHIM(msvcr71, _c_exit, kCdecl) { return 0; }
SHIM(msvcr71, _amsg_exit, kCdecl) { fatal("_amsg_exit(%u)", arg(c, 0)); }
SHIM(msvcr71, _XcptFilter, kCdecl) { return 0; }
SHIM(msvcr71, _except_handler3, kCdecl) { fatal("structured exception raised"); }
SHIM(msvcr71, __CxxFrameHandler, kCdecl) { fatal("C++ exception handler reached"); }
SHIM(msvcr71, _CxxThrowException, 8) { fatal("C++ exception thrown (object %08x, type info %08x)", arg(c, 0), arg(c, 1)); }
SHIM_AS(msvcr71, terminate, "?terminate@@YAXXZ", kCdecl) { fatal("terminate()"); }
SHIM(msvcr71, _purecall, kCdecl) { fatal("pure virtual call"); }
SHIM(msvcr71, __security_error_handler, kCdecl) { fatal("buffer overrun detected (code %u)", arg(c, 0)); }
SHIM(msvcr71, _callnewh, kCdecl) { return 0; }
SHIM(msvcr71, _errno, kCdecl) { return errno_block(); }

// class exception { vptr; const char *what; int doFree; }
static uint32_t exception_vtable() {
    static uint32_t vt = 0;
    if (!vt) {
        vt = galloc(8);
        st32(vt, make_thunk("exception::~exception (deleting)", 4, [](CPU *c) { return c->r[1]; }));
        st32(vt + 4, make_thunk("exception::what", 0, [](CPU *c) {
                 uint32_t w = ld32(c->r[1] + 4);
                 static uint32_t unknown = gstrdup("Unknown exception");
                 return w ? w : unknown;
             }));
    }
    return vt;
}
SHIM_AS(msvcr71, exception_ctor, "??0exception@@QAE@XZ", 0) {
    st32(c->r[1], exception_vtable());
    st32(c->r[1] + 4, 0);
    st32(c->r[1] + 8, 0);
    return c->r[1];
}
SHIM_AS(msvcr71, exception_copy, "??0exception@@QAE@ABV0@@Z", 4) {
    uint32_t o = arg(c, 0);
    st32(c->r[1], exception_vtable());
    st32(c->r[1] + 4, ld32(o + 4) ? gstrdup(gstr(ld32(o + 4))) : 0);
    st32(c->r[1] + 8, ld32(o + 4) ? 1 : 0);
    return c->r[1];
}
SHIM_AS(msvcr71, exception_dtor, "??1exception@@UAE@XZ", 0) { return 0; }
SHIM_AS(msvcr71, type_info_dtor, "??1type_info@@UAE@XZ", 0) { return 0; }

// ---- memory ----
SHIM(msvcr71, malloc, kCdecl) { return galloc(arg(c, 0)); }
SHIM(msvcr71, calloc, kCdecl) { return galloc(arg(c, 0) * arg(c, 1), true); }
SHIM(msvcr71, realloc, kCdecl) { return grealloc(arg(c, 0), arg(c, 1)); }
SHIM(msvcr71, free, kCdecl) {
    gfree(arg(c, 0));
    return 0;
}
SHIM_AS(msvcr71, op_delete, "??3@YAXPAX@Z", kCdecl) {
    gfree(arg(c, 0));
    return 0;
}
SHIM_AS(msvcr71, op_delete_array, "??_V@YAXPAX@Z", kCdecl) {
    gfree(arg(c, 0));
    return 0;
}
SHIM(msvcr71, memset, kCdecl) {
    memset(GM + arg(c, 0), int(arg(c, 1)), arg(c, 2));
    return arg(c, 0);
}
SHIM(msvcr71, memcpy, kCdecl) {
    memmove(GM + arg(c, 0), GM + arg(c, 1), arg(c, 2));
    return arg(c, 0);
}
SHIM(msvcr71, memmove, kCdecl) {
    memmove(GM + arg(c, 0), GM + arg(c, 1), arg(c, 2));
    return arg(c, 0);
}
SHIM(msvcr71, memcmp, kCdecl) { return uint32_t(memcmp(GM + arg(c, 0), GM + arg(c, 1), arg(c, 2))); }
SHIM(msvcr71, memchr, kCdecl) {
    const void *p = memchr(GM + arg(c, 0), int(arg(c, 1)), arg(c, 2));
    return p ? gaddr(p) : 0;
}

// ---- strings ----
SHIM(msvcr71, strlen, kCdecl) { return uint32_t(strlen(gstr(arg(c, 0)))); }
SHIM(msvcr71, wcslen, kCdecl) {
    uint32_t a = arg(c, 0), n = 0;
    while (ld16(a + 2 * n)) n++;
    return n;
}
SHIM(msvcr71, strcpy, kCdecl) {
    strcpy(gptr(arg(c, 0)), gstr(arg(c, 1)));
    return arg(c, 0);
}
SHIM(msvcr71, strncpy, kCdecl) {
    strncpy(gptr(arg(c, 0)), gstr(arg(c, 1)), arg(c, 2));
    return arg(c, 0);
}
SHIM(msvcr71, strcat, kCdecl) {
    strcat(gptr(arg(c, 0)), gstr(arg(c, 1)));
    return arg(c, 0);
}
SHIM(msvcr71, strcmp, kCdecl) { return uint32_t(strcmp(gstr(arg(c, 0)), gstr(arg(c, 1)))); }
SHIM(msvcr71, strncmp, kCdecl) { return uint32_t(strncmp(gstr(arg(c, 0)), gstr(arg(c, 1)), arg(c, 2))); }
SHIM(msvcr71, _stricmp, kCdecl) { return uint32_t(strcasecmp(gstr(arg(c, 0)), gstr(arg(c, 1)))); }
SHIM(msvcr71, _strnicmp, kCdecl) { return uint32_t(strncasecmp(gstr(arg(c, 0)), gstr(arg(c, 1)), arg(c, 2))); }
SHIM(msvcr71, strchr, kCdecl) {
    const char *p = strchr(gstr(arg(c, 0)), int(arg(c, 1)));
    return p ? gaddr(p) : 0;
}
SHIM(msvcr71, strrchr, kCdecl) {
    const char *p = strrchr(gstr(arg(c, 0)), int(arg(c, 1)));
    return p ? gaddr(p) : 0;
}
SHIM(msvcr71, atoi, kCdecl) { return uint32_t(atoi(gstr(arg(c, 0)))); }
SHIM(msvcr71, toupper, kCdecl) { return uint32_t(toupper(int(arg(c, 0)))); }
SHIM(msvcr71, abs, kCdecl) { return uint32_t(abs(int32_t(arg(c, 0)))); }
SHIM(msvcr71, labs, kCdecl) { return uint32_t(abs(int32_t(arg(c, 0)))); }
SHIM(msvcr71, _ismbblead, kCdecl) {  // code page 932 (Shift-JIS)
    uint32_t b = arg(c, 0) & 0xFF;
    return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC);
}
SHIM(msvcr71, _splitpath, kCdecl) {
    std::string p = gstr(arg(c, 0));
    std::string drive, dir, fname, ext;
    size_t i = 0;
    if (p.size() >= 2 && p[1] == ':') {
        drive = p.substr(0, 2);
        i = 2;
    }
    size_t slash = p.find_last_of("\\/");
    if (slash != std::string::npos && slash >= i) {
        dir = p.substr(i, slash + 1 - i);
        i = slash + 1;
    }
    std::string rest = p.substr(i);
    size_t dot = rest.find_last_of('.');
    if (dot != std::string::npos) {
        fname = rest.substr(0, dot);
        ext = rest.substr(dot);
    } else {
        fname = rest;
    }
    auto put = [&](uint32_t a, const std::string &s) {
        if (a) memcpy(GM + a, s.c_str(), s.size() + 1);
    };
    put(arg(c, 1), drive);
    put(arg(c, 2), dir);
    put(arg(c, 3), fname);
    put(arg(c, 4), ext);
    return 0;
}

// ---- formatting ----
SHIM(msvcr71, sprintf, kCdecl) {
    std::string s;
    format_guest(s, gstr(arg(c, 1)), c->r[4] + 12);
    memcpy(GM + arg(c, 0), s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}
SHIM(msvcr71, _vsnprintf, kCdecl) {
    std::string s;
    format_guest(s, gstr(arg(c, 2)), arg(c, 3));
    uint32_t n = arg(c, 1);
    if (s.size() >= n) {
        memcpy(GM + arg(c, 0), s.data(), n);
        return 0xFFFFFFFF;
    }
    memcpy(GM + arg(c, 0), s.c_str(), s.size() + 1);
    return uint32_t(s.size());
}
SHIM(msvcr71, swprintf, kCdecl) {
    std::string fmt;
    for (uint32_t a = arg(c, 1); ld16(a); a += 2) fmt += char(ld16(a));
    std::string s;
    format_guest(s, fmt.c_str(), c->r[4] + 12, true);
    uint32_t d = arg(c, 0);
    for (size_t i = 0; i <= s.size(); i++) st16(d + 2 * uint32_t(i), i < s.size() ? uint8_t(s[i]) : 0);
    return uint32_t(s.size());
}
SHIM(msvcr71, printf, kCdecl) {
    std::string s;
    format_guest(s, gstr(arg(c, 0)), c->r[4] + 8);
    fputs(s.c_str(), stdout);
    return uint32_t(s.size());
}
SHIM(msvcr71, fprintf, kCdecl) {
    std::string s;
    format_guest(s, gstr(arg(c, 1)), c->r[4] + 12);
    FILE *f = host_file(arg(c, 0));
    fputs(s.c_str(), f ? f : stderr);
    return uint32_t(s.size());
}
SHIM(msvcr71, perror, kCdecl) {
    log("perror: %s", gstr(arg(c, 0)));
    return 0;
}

// ---- files ----
SHIM(msvcr71, fopen, kCdecl) {
    std::string path = host_path(gstr(arg(c, 0)));
    std::string mode = gstr(arg(c, 1));
    FILE *f = path.empty() ? nullptr : fopen(path.c_str(), mode.c_str());
    if (trace_imports) log("  fopen %s (%s) -> %s", gstr(arg(c, 0)), mode.c_str(), f ? "ok" : "failed");
    if (!f) return 0;
    uint32_t g = galloc(32, true);
    std::lock_guard<std::mutex> lock(file_mutex);
    files[g] = f;
    return g;
}
SHIM(msvcr71, fclose, kCdecl) {
    FILE *f = host_file(arg(c, 0));
    if (!f) return 0xFFFFFFFF;
    fclose(f);
    {
        std::lock_guard<std::mutex> lock(file_mutex);
        files.erase(arg(c, 0));
    }
    gfree(arg(c, 0));
    return 0;
}
SHIM(msvcr71, fread, kCdecl) {
    FILE *f = host_file(arg(c, 3));
    return f ? uint32_t(fread(GM + arg(c, 0), arg(c, 1), arg(c, 2), f)) : 0;
}
SHIM(msvcr71, fgets, kCdecl) {
    FILE *f = host_file(arg(c, 2));
    return f && fgets(gptr(arg(c, 0)), int(arg(c, 1)), f) ? arg(c, 0) : 0;
}
SHIM(msvcr71, fseek, kCdecl) {
    FILE *f = host_file(arg(c, 0));
    return f ? uint32_t(fseek(f, long(int32_t(arg(c, 1))), int(arg(c, 2)))) : 0xFFFFFFFF;
}
SHIM(msvcr71, ftell, kCdecl) {
    FILE *f = host_file(arg(c, 0));
    return f ? uint32_t(ftell(f)) : 0xFFFFFFFF;
}

// ---- math (doubles on the stack; results in ST(0)) ----
SHIM(msvcr71, sqrt, kCdecl) { return ret_double(c, sqrt(darg(c, 0))); }
SHIM(msvcr71, fabs, kCdecl) { return ret_double(c, fabs(darg(c, 0))); }
SHIM(msvcr71, cos, kCdecl) { return ret_double(c, cos(darg(c, 0))); }
SHIM(msvcr71, asin, kCdecl) { return ret_double(c, asin(darg(c, 0))); }
SHIM(msvcr71, acos, kCdecl) { return ret_double(c, acos(darg(c, 0))); }
SHIM(msvcr71, atan2, kCdecl) { return ret_double(c, atan2(darg(c, 0), darg(c, 2))); }
SHIM(msvcr71, pow, kCdecl) { return ret_double(c, pow(darg(c, 0), darg(c, 2))); }
SHIM(msvcr71, ceil, kCdecl) { return ret_double(c, ceil(darg(c, 0))); }
SHIM(msvcr71, floor, kCdecl) { return ret_double(c, floor(darg(c, 0))); }
SHIM(msvcr71, ldexp, kCdecl) { return ret_double(c, ldexp(darg(c, 0), int(arg(c, 2)))); }
SHIM(msvcr71, frexp, kCdecl) {
    int e;
    double m = frexp(darg(c, 0), &e);
    st32(arg(c, 2), uint32_t(e));
    return ret_double(c, m);
}
// _CI* take their operands on the x87 stack.
SHIM(msvcr71, _CIpow, kCdecl) {
    double y = ST(0), x = ST(1);
    FPOP();
    ST(0) = pow(x, y);
    return c->r[0];
}
SHIM(msvcr71, _CIacos, kCdecl) {
    ST(0) = acos(ST(0));
    return c->r[0];
}
SHIM(msvcr71, _ftol, kCdecl) {
    double x = ST(0);
    FPOP();
    int64_t v = (x != x || fabs(x) >= 9.2e18) ? INT64_MIN : int64_t(x);
    c->r[2] = uint32_t(uint64_t(v) >> 32);
    return uint32_t(v);
}

SHIM(msvcr71, qsort, kCdecl) {
    uint32_t base = arg(c, 0), n = arg(c, 1), w = arg(c, 2), cmp = arg(c, 3);
    // Insertion sort through the guest comparator (stable and simple; lists here are short).
    std::vector<uint8_t> tmp(w);
    for (uint32_t i = 1; i < n; i++) {
        memcpy(tmp.data(), GM + base + i * w, w);
        uint32_t key = galloc(w);
        memcpy(GM + key, tmp.data(), w);
        uint32_t j = i;
        while (j > 0 && int32_t(call_guest(c, cmp, {base + (j - 1) * w, key})) > 0) {
            memcpy(GM + base + j * w, GM + base + (j - 1) * w, w);
            j--;
        }
        memcpy(GM + base + j * w, GM + key, w);
        gfree(key);
    }
    return 0;
}

// ---- msvcp71: std::string (VC7.1 layout: allocator word, 16-byte buffer/pointer, size, reserved) ----
namespace {
uint32_t str_data(uint32_t s) { return ld32(s + 24) >= 16 ? ld32(s + 4) : s + 4; }
void str_assign(uint32_t s, const char *p, uint32_t n) {
    if (n < 16) {
        memcpy(GM + s + 4, p, n);
        st8(s + 4 + n, 0);
        st32(s + 24, 15);
    } else {
        uint32_t b = galloc(n + 1);
        memcpy(GM + b, p, n);
        st8(b + n, 0);
        st32(s + 4, b);
        st32(s + 24, n);
    }
    st32(s + 20, n);
}
}  // namespace
SHIM_AS(msvcp71, string_ctor_cstr, "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@PBD@Z", 4) {
    const char *p = gstr(arg(c, 0));
    std::string tmp(p);  // the source may live in guest memory we are about to allocate
    str_assign(c->r[1], tmp.data(), uint32_t(tmp.size()));
    return c->r[1];
}
SHIM_AS(msvcp71, string_ctor_copy, "??0?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@ABV01@@Z", 4) {
    uint32_t o = arg(c, 0);
    std::string tmp(gstr(str_data(o)), ld32(o + 20));
    str_assign(c->r[1], tmp.data(), uint32_t(tmp.size()));
    return c->r[1];
}
SHIM_AS(msvcp71, string_dtor, "??1?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QAE@XZ", 0) {
    uint32_t s = c->r[1];
    if (ld32(s + 24) >= 16) gfree(ld32(s + 4));
    st32(s + 24, 15);
    st32(s + 20, 0);
    st8(s + 4, 0);
    return 0;
}
SHIM_AS(msvcp71, string_c_str, "?c_str@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QBEPBDXZ", 0) {
    return str_data(c->r[1]);
}
SHIM_AS(msvcp71, nomemory, "?_Nomemory@std@@YAXXZ", kCdecl) { fatal("std::_Nomemory (out of memory)"); }
