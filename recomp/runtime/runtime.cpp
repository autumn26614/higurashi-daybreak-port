// Guest memory, loader, dispatcher, heap and threads for the recompiled program.
#include "runtime.h"

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

uint8_t *GM = nullptr;

namespace rt {

bool trace_imports = getenv("RECOMP_TRACE") != nullptr;

void log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("recomp: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    if (CPU *c = current()) {
        fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->r[0],
                c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6], c->r[7]);
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", ld32(c->r[4] + 4 * i));
        fputc('\n', stderr);
    }
    fflush(stderr);
    abort();
}

// ---- memory ----
void memory_init() {
    if (GM) return;
    void *p = mmap(nullptr, size_t(1) << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1, 0);
    if (p == MAP_FAILED) fatal("cannot reserve the 4 GiB guest address space");
    GM = static_cast<uint8_t *>(p);
}

// Heap: size classes (16-byte steps to 1 KiB, then powers of two), bump allocation, free
// lists per class. Each block has an 8-byte header holding its class size.
namespace {
std::mutex heap_mutex;
uint32_t heap_top = kHeapBase;
std::map<uint32_t, std::vector<uint32_t>> free_lists;

uint32_t class_size(uint32_t n) {
    if (n <= 1024) return (n + 15) & ~15u;
    uint32_t s = 2048;
    while (s < n) s <<= 1;
    return s;
}
}  // namespace

uint32_t galloc(uint32_t size, bool zero) {
    uint32_t cs = class_size(size ? size : 1);
    uint32_t a;
    {
        std::lock_guard<std::mutex> lock(heap_mutex);
        auto &fl = free_lists[cs];
        if (!fl.empty()) {
            a = fl.back();
            fl.pop_back();
        } else {
            a = heap_top + 16;  // header in the 16 bytes before, keeps 16-byte alignment
            if (uint64_t(a) + cs > kHeapEnd) fatal("guest heap exhausted (%u bytes)", size);
            heap_top = a + cs;
            st32(a - 8, cs);
        }
    }
    if (zero) memset(GM + a, 0, cs);
    return a;
}

void gfree(uint32_t a) {
    if (!a) return;
    if (a < kHeapBase || a >= kHeapEnd) return;  // not ours (static data, stack)
    std::lock_guard<std::mutex> lock(heap_mutex);
    free_lists[ld32(a - 8)].push_back(a);
}

uint32_t gsize(uint32_t a) { return a ? ld32(a - 8) : 0; }

uint32_t grealloc(uint32_t a, uint32_t size) {
    if (!a) return galloc(size);
    if (!size) {
        gfree(a);
        return 0;
    }
    uint32_t old = gsize(a);
    if (size <= old) return a;
    uint32_t b = galloc(size);
    memcpy(GM + b, GM + a, old);
    gfree(a);
    return b;
}

uint32_t gstrdup(const std::string &s) {
    uint32_t a = galloc(uint32_t(s.size() + 1));
    memcpy(GM + a, s.c_str(), s.size() + 1);
    return a;
}

// ---- thunks ----
namespace {
struct Thunk {
    std::string name;
    int stack_bytes;  // kCdecl, or bytes popped; -2 = not implemented
    ShimFn fn;
};
std::vector<Thunk> &thunks() {
    static std::vector<Thunk> t;
    return t;
}
std::mutex thunk_mutex;

std::string lower(std::string s) {
    for (auto &ch : s) ch = char(tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string module_key(std::string dll) {
    dll = lower(dll);
    if (dll.size() > 4 && dll.compare(dll.size() - 4, 4, ".dll") == 0) dll.resize(dll.size() - 4);
    return dll;
}
struct ShimDef {
    int stack_bytes;
    ShimFn fn;
    uint32_t (*data)();
};
std::map<std::string, ShimDef> &registry() {
    static std::map<std::string, ShimDef> r;
    return r;
}
}  // namespace

ShimReg::ShimReg(const char *dll, const char *name, int stack_bytes, ShimFn fn) {
    registry()[module_key(dll) + "!" + name] = {stack_bytes, fn, nullptr};
}
DataReg::DataReg(const char *dll, const char *name, uint32_t (*make)()) {
    registry()[module_key(dll) + "!" + name] = {0, nullptr, make};
}

uint32_t make_thunk(const std::string &name, int stack_bytes, ShimFn fn) {
    std::lock_guard<std::mutex> lock(thunk_mutex);
    thunks().push_back({name, stack_bytes, fn});
    return kThunkBase + 4 * uint32_t(thunks().size() - 1);
}

uint32_t resolve_import(const std::string &dll, const std::string &name) {
    std::string key = module_key(dll) + "!" + name;
    auto it = registry().find(key);
    if (it == registry().end()) return 0;
    if (it->second.data) return it->second.data();
    static std::map<std::string, uint32_t> made;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    uint32_t &t = made[key];
    if (!t) t = make_thunk(key, it->second.stack_bytes, it->second.fn);
    return t;
}

static void call_thunk(CPU *c, uint32_t t) {
    uint32_t i = (t - kThunkBase) / 4;
    Thunk k;
    {
        std::lock_guard<std::mutex> lock(thunk_mutex);
        if (i >= thunks().size()) fatal("bad thunk address %08x", t);
        k = thunks()[i];
    }
    if (!k.fn) fatal("call to unimplemented import %s (return address %08x)", k.name.c_str(), ld32(c->r[4]));
    if (trace_imports) log("[%u] %s", thread_id(), k.name.c_str());
    uint32_t r = k.fn(c);
    c->r[0] = r;
    c->r[4] += 4 + (k.stack_bytes > 0 ? uint32_t(k.stack_bytes) : 0);
}

// ---- dispatch ----
namespace {
const FnEntry *g_table = nullptr;
uint32_t g_count = 0;
}  // namespace

GuestFn lookup(uint32_t addr) {
    const FnEntry *e = std::lower_bound(g_table, g_table + g_count, addr,
                                        [](const FnEntry &x, uint32_t a) { return x.addr < a; });
    if (e != g_table + g_count && e->addr == addr) return e->fn;
    return nullptr;
}

}  // namespace rt

extern "C" void rt_icall(CPU *c, uint32_t t) {
    if (t >= rt::kThunkBase) {
        rt::call_thunk(c, t);
        return;
    }
    GuestFn f = rt::lookup(t);
    if (!f) rt::fatal("no translated function at %08x (called from around %08x)", t, ld32(c->r[4]));
    f(c);
}

extern "C" void rt_trap(CPU *c, uint32_t addr, const char *what) {
    (void)c;
    rt::fatal("trap at %08x: %s", addr, what);
}

extern "C" double ldf80(uint32_t a) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, GM + a, 8);
    memcpy(&se, GM + a + 8, 2);
    int e = se & 0x7FFF;
    double sign = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && mant == 0) return 0.0 * sign;
    if (e == 0x7FFF) return (mant << 1) ? NAN : sign * INFINITY;
    return sign * ldexp(double(mant), e - 16383 - 63);
}

extern "C" void stf80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    v = fabs(v);
    if (v != v) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (v != 0) {
        int e;
        double m = frexp(v, &e);  // v = m * 2^e, m in [0.5, 1)
        mant = uint64_t(ldexp(m, 64));
        se |= uint16_t(e - 1 + 16383);
    }
    memcpy(GM + a, &mant, 8);
    memcpy(GM + a + 8, &se, 2);
}

namespace rt {

// ---- calling guest code ----
uint32_t call_guest(CPU *c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t ecx) {
    uint32_t saved = c->r[4];
    uint32_t sp = saved;
    for (auto it = args.end(); it != args.begin();) {
        --it;
        sp -= 4;
        st32(sp, *it);
    }
    sp -= 4;
    st32(sp, 0);  // return address: never used (translated code returns to the host)
    c->r[4] = sp;
    if (ecx) c->r[1] = ecx;
    rt_icall(c, fn);
    c->r[4] = saved;
    return c->r[0];
}

// ---- threads ----
namespace {
thread_local CPU *t_cpu = nullptr;
std::atomic<uint32_t> next_tid{0x100};
thread_local uint32_t t_tid = 0;
}  // namespace

CPU *current() { return t_cpu; }

uint32_t thread_id() {
    if (!t_tid) t_tid = next_tid.fetch_add(4);
    return t_tid;
}

CPU *new_cpu(uint32_t stack_size) {
    CPU *c = new CPU();
    uint32_t stack = galloc(stack_size);
    uint32_t tib = galloc(0x1000, true);
    st32(tib + 0x00, 0xFFFFFFFF);         // SEH chain end
    st32(tib + 0x04, stack + stack_size);  // stack base
    st32(tib + 0x08, stack);               // stack limit
    st32(tib + 0x18, tib);                 // self
    st32(tib + 0x24, thread_id());
    st32(tib + 0x30, tib + 0x800);         // a zeroed stand-in PEB
    c->fs_base = tib;
    c->r[4] = stack + stack_size - 64;
    c->fcw = 0x027F;                       // what the process starts with (53-bit, nearest)
    t_cpu = c;
    return c;
}

void free_cpu(CPU *c) {
    if (t_cpu == c) t_cpu = nullptr;
    delete c;
}

// ---- loading ----
uint32_t load_exe(const std::string &exe, const Program &p) {
    memory_init();
    g_table = p.table;
    g_count = p.count;
    std::ifstream in(exe, std::ios::binary);
    if (!in) fatal("cannot open %s", exe.c_str());
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return uint32_t(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | uint32_t(d[o + 3]) << 24); };
    uint32_t pe = u32(0x3C);
    uint32_t nsec = u16(pe + 6), opt_size = u16(pe + 20);
    uint32_t opt = pe + 24;
    uint32_t entry = u32(opt + 16), base = u32(opt + 28);
    uint32_t headers = u32(opt + 60);
    memcpy(GM + base, d.data(), std::min<size_t>(headers, d.size()));
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t o = opt + opt_size + i * 40;
        uint32_t vsize = u32(o + 8), va = u32(o + 12), raw_size = u32(o + 16), raw = u32(o + 20);
        uint32_t n = std::min(raw_size, vsize ? vsize : raw_size);
        if (raw + n > d.size()) n = uint32_t(d.size() - raw);
        memcpy(GM + base + va, d.data() + raw, n);
    }
    // Bind imports.
    int missing = 0;
    for (uint32_t i = 0; i < p.import_count; i++) {
        const ImportSlot &s = p.imports[i];
        std::string key = module_key(s.dll) + "!" + s.name;
        auto it = registry().find(key);
        uint32_t v;
        if (it == registry().end()) {
            v = make_thunk(key, -2, nullptr);
            missing++;
        } else if (it->second.data) {
            v = it->second.data();
        } else {
            v = make_thunk(key, it->second.stack_bytes, it->second.fn);
        }
        st32(s.iat, v);
    }
    log("recomp: loaded %s at %08x, %u functions, %u imports (%d without a shim)", exe.c_str(), base, p.count,
        p.import_count, missing);
    return base + entry;
}

}  // namespace rt
