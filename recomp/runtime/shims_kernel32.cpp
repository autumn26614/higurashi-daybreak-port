// kernel32: synchronisation, threads, files, time and module queries.
#include <iconv.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <unistd.h>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "host.h"
#include "runtime.h"

using namespace rt;

namespace {
thread_local uint32_t last_error = 0;
constexpr uint32_t kInfinite = 0xFFFFFFFF;
constexpr uint32_t kWaitTimeout = 0x102;
constexpr uint32_t kPseudoProcess = 0xFFFFFFFF, kPseudoThread = 0xFFFFFFFE;

struct Event : KObject {
    std::mutex m;
    std::condition_variable cv;
    bool manual, state;
    Event(bool manual_, bool state_) : manual(manual_), state(state_) {}
    bool wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lock(m);
        auto ready = [&] { return state; };
        if (ms == kInfinite) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) return false;
        if (!manual) state = false;
        return true;
    }
    void set(bool v) {
        std::lock_guard<std::mutex> lock(m);
        state = v;
        if (v) cv.notify_all();
    }
};

struct Semaphore : KObject {
    std::mutex m;
    std::condition_variable cv;
    int32_t count, max;
    Semaphore(int32_t n, int32_t mx) : count(n), max(mx) {}
    bool wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lock(m);
        auto ready = [&] { return count > 0; };
        if (ms == kInfinite) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) return false;
        count--;
        return true;
    }
};

struct Thread : KObject {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    uint32_t exit_code = 259;  // STILL_ACTIVE
    uint32_t tid = 0;
    bool wait(uint32_t ms) override {
        std::unique_lock<std::mutex> lock(m);
        auto ready = [&] { return done; };
        if (ms == kInfinite) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) return false;
        return true;
    }
};

struct File : KObject {
    FILE *f = nullptr;
    ~File() override {
        if (f) fclose(f);
    }
};

// Critical sections are guest structs; each gets a host recursive mutex.
std::mutex cs_mutex;
std::map<uint32_t, std::recursive_mutex *> sections;
std::recursive_mutex *section(uint32_t a) {
    std::lock_guard<std::mutex> lock(cs_mutex);
    auto &m = sections[a];
    if (!m) m = new std::recursive_mutex();
    return m;
}

uint32_t wait_one(uint32_t h, uint32_t ms) {
    auto o = handle_get(h);
    if (!o) {
        last_error = 6;
        return 0xFFFFFFFF;
    }
    return o->wait(ms) ? 0 : kWaitTimeout;
}
}  // namespace

void rt_set_event(uint32_t h) {  // for the DirectSound notifications
    if (auto e = handle_as<Event>(h)) e->set(true);
}

// ---- synchronisation ----
SHIM(kernel32, InitializeCriticalSection, 4) {
    section(arg(c, 0));
    return 0;
}
SHIM(kernel32, DeleteCriticalSection, 4) {
    std::lock_guard<std::mutex> lock(cs_mutex);
    auto it = sections.find(arg(c, 0));
    if (it != sections.end()) {
        delete it->second;
        sections.erase(it);
    }
    return 0;
}
SHIM(kernel32, EnterCriticalSection, 4) {
    section(arg(c, 0))->lock();
    return 0;
}
SHIM(kernel32, LeaveCriticalSection, 4) {
    section(arg(c, 0))->unlock();
    return 0;
}
SHIM(kernel32, CreateEventA, 16) { return handle_new(std::make_shared<Event>(arg(c, 1) != 0, arg(c, 2) != 0)); }
SHIM(kernel32, SetEvent, 4) {
    if (auto e = handle_as<Event>(arg(c, 0))) e->set(true);
    return 1;
}
SHIM(kernel32, ResetEvent, 4) {
    if (auto e = handle_as<Event>(arg(c, 0))) e->set(false);
    return 1;
}
SHIM(kernel32, CreateSemaphoreA, 16) {
    return handle_new(std::make_shared<Semaphore>(int32_t(arg(c, 1)), int32_t(arg(c, 2))));
}
SHIM(kernel32, ReleaseSemaphore, 12) {
    auto s = handle_as<Semaphore>(arg(c, 0));
    if (!s) return 0;
    std::lock_guard<std::mutex> lock(s->m);
    if (arg(c, 2)) st32(arg(c, 2), uint32_t(s->count));
    s->count += int32_t(arg(c, 1));
    if (s->count > s->max) s->count = s->max;
    s->cv.notify_all();
    return 1;
}
SHIM(kernel32, WaitForSingleObject, 8) { return wait_one(arg(c, 0), arg(c, 1)); }
SHIM(kernel32, WaitForMultipleObjects, 16) {
    uint32_t n = arg(c, 0), list = arg(c, 1), all = arg(c, 2), ms = arg(c, 3);
    auto deadline = now_us() + uint64_t(ms) * 1000;
    if (all) {
        for (uint32_t i = 0; i < n; i++)
            if (wait_one(ld32(list + 4 * i), ms) == kWaitTimeout) return kWaitTimeout;
        return 0;
    }
    for (;;) {
        for (uint32_t i = 0; i < n; i++)
            if (wait_one(ld32(list + 4 * i), 0) == 0) return i;
        if (ms != kInfinite && now_us() >= deadline) return kWaitTimeout;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
SHIM(kernel32, InterlockedExchange, 8) { return __atomic_exchange_n(reinterpret_cast<uint32_t *>(GM + arg(c, 0)), arg(c, 1), __ATOMIC_SEQ_CST); }
SHIM(kernel32, InterlockedIncrement, 4) { return __atomic_add_fetch(reinterpret_cast<uint32_t *>(GM + arg(c, 0)), 1u, __ATOMIC_SEQ_CST); }
SHIM(kernel32, InterlockedDecrement, 4) { return __atomic_sub_fetch(reinterpret_cast<uint32_t *>(GM + arg(c, 0)), 1u, __ATOMIC_SEQ_CST); }

// ---- threads ----
SHIM(kernel32, CreateThread, 24) {
    uint32_t start = arg(c, 2), param = arg(c, 3), tid_out = arg(c, 5);
    auto t = std::make_shared<Thread>();
    uint32_t h = handle_new(t);
    std::mutex started;
    std::condition_variable cv;
    bool ready = false;
    std::thread([t, start, param, &started, &cv, &ready] {
        CPU *tc = new_cpu(kThreadStack);
        {
            std::lock_guard<std::mutex> lock(started);
            t->tid = thread_id();
            ready = true;
            cv.notify_all();
        }
        uint32_t code = call_guest(tc, start, {param});
        {
            std::lock_guard<std::mutex> lock(t->m);
            t->exit_code = code;
            t->done = true;
        }
        t->cv.notify_all();
        free_cpu(tc);
    }).detach();
    std::unique_lock<std::mutex> lock(started);
    cv.wait(lock, [&] { return ready; });
    if (tid_out) st32(tid_out, t->tid);
    return h;
}
SHIM(kernel32, GetExitCodeThread, 8) {
    auto t = handle_as<Thread>(arg(c, 0));
    if (!t) return 0;
    std::lock_guard<std::mutex> lock(t->m);
    st32(arg(c, 1), t->exit_code);
    return 1;
}
SHIM(kernel32, SetThreadPriority, 8) { return 1; }
SHIM(kernel32, GetThreadPriority, 4) { return 0; }
SHIM(kernel32, GetCurrentThread, 0) { return kPseudoThread; }
SHIM(kernel32, GetCurrentProcess, 0) { return kPseudoProcess; }
SHIM(kernel32, GetCurrentThreadId, 0) { return thread_id(); }
SHIM(kernel32, GetCurrentProcessId, 0) { return 0x10; }
SHIM(kernel32, DuplicateHandle, 28) {
    uint32_t src = arg(c, 1), out = arg(c, 3);
    std::shared_ptr<KObject> o;
    if (src == kPseudoThread) {
        auto t = std::make_shared<Thread>();
        t->tid = thread_id();
        o = t;
    } else {
        o = handle_get(src);
    }
    if (!o) return 0;
    if (out) st32(out, handle_new(o));
    return 1;
}
SHIM(kernel32, Sleep, 4) {
    std::this_thread::sleep_for(std::chrono::milliseconds(arg(c, 0)));
    return 0;
}
SHIM(kernel32, ExitProcess, 4) {
    log("recomp: ExitProcess(%u)", arg(c, 0));
    fflush(nullptr);
    _exit(int(arg(c, 0)));
}

// ---- files ----
SHIM(kernel32, CreateFileA, 28) {
    std::string name = gstr(arg(c, 0));
    uint32_t access = arg(c, 1), disposition = arg(c, 4);
    std::string path = host_path(name);
    const char *mode = "rb";
    if (access & 0x40000000) {  // GENERIC_WRITE
        struct stat st;
        bool exists = stat(path.c_str(), &st) == 0;
        if (disposition == 2 || disposition == 1 || (!exists && (disposition == 4))) mode = "w+b";
        else mode = "r+b";
    }
    FILE *f = path.empty() ? nullptr : fopen(path.c_str(), mode);
    if (trace_imports) log("  CreateFileA %s (%s) -> %s", name.c_str(), mode, f ? "ok" : "failed");
    if (!f) {
        last_error = 2;  // ERROR_FILE_NOT_FOUND
        return 0xFFFFFFFF;
    }
    auto o = std::make_shared<File>();
    o->f = f;
    return handle_new(o);
}
SHIM(kernel32, ReadFile, 20) {
    auto f = handle_as<File>(arg(c, 0));
    if (!f) return 0;
    size_t n = fread(GM + arg(c, 1), 1, arg(c, 2), f->f);
    if (arg(c, 3)) st32(arg(c, 3), uint32_t(n));
    return 1;
}
SHIM(kernel32, WriteFile, 20) {
    auto f = handle_as<File>(arg(c, 0));
    if (!f) return 0;
    size_t n = fwrite(GM + arg(c, 1), 1, arg(c, 2), f->f);
    if (arg(c, 3)) st32(arg(c, 3), uint32_t(n));
    return 1;
}
SHIM(kernel32, GetFileSize, 8) {
    auto f = handle_as<File>(arg(c, 0));
    if (!f) return 0xFFFFFFFF;
    long pos = ftell(f->f);
    fseek(f->f, 0, SEEK_END);
    long size = ftell(f->f);
    fseek(f->f, pos, SEEK_SET);
    if (arg(c, 1)) st32(arg(c, 1), 0);
    return uint32_t(size);
}
SHIM(kernel32, SetFilePointer, 16) {
    auto f = handle_as<File>(arg(c, 0));
    if (!f) return 0xFFFFFFFF;
    int64_t dist = int32_t(arg(c, 1));
    if (arg(c, 2)) dist = int64_t((uint64_t(ld32(arg(c, 2))) << 32) | arg(c, 1));
    int whence = arg(c, 3) == 1 ? SEEK_CUR : arg(c, 3) == 2 ? SEEK_END : SEEK_SET;
    fseek(f->f, long(dist), whence);
    long pos = ftell(f->f);
    if (arg(c, 2)) st32(arg(c, 2), 0);
    return uint32_t(pos);
}
SHIM(kernel32, CloseHandle, 4) { return handle_close(arg(c, 0)) ? 1 : 0; }

// ---- process and modules ----
SHIM(kernel32, GetModuleHandleA, 4) { return arg(c, 0) ? 0 : 0x400000; }
SHIM(kernel32, GetModuleFileNameA, 12) {
    std::string s = std::string(kGuestDir) + "\\daybreak.exe";
    uint32_t n = arg(c, 2);
    if (!n) return 0;
    uint32_t len = std::min<uint32_t>(uint32_t(s.size()), n - 1);
    memcpy(GM + arg(c, 1), s.c_str(), len);
    st8(arg(c, 1) + len, 0);
    return len;
}
SHIM(kernel32, SetCurrentDirectoryA, 4) {
    set_guest_cwd(gstr(arg(c, 0)));
    return 1;
}
SHIM(kernel32, GetStartupInfoA, 4) {
    memset(GM + arg(c, 0), 0, 68);
    st32(arg(c, 0), 68);
    return 0;
}
SHIM(kernel32, GetVersionExA, 4) {  // Windows XP SP2
    uint32_t o = arg(c, 0);
    st32(o + 4, 5);
    st32(o + 8, 1);
    st32(o + 12, 2600);
    st32(o + 16, 2);
    strcpy(gptr(o + 20), "Service Pack 2");
    return 1;
}
SHIM(kernel32, GetLastError, 0) { return last_error; }
SHIM(kernel32, GetSystemInfo, 4) {
    uint32_t o = arg(c, 0);
    memset(GM + o, 0, 36);
    st32(o + 4, 4096);         // page size
    st32(o + 8, 0x10000);      // lowest application address
    st32(o + 12, 0x7FFEFFFF);  // highest
    st32(o + 16, 1);           // active processor mask
    st32(o + 20, 1);           // processors
    st32(o + 24, 586);         // processor type
    st32(o + 28, 0x10000);     // allocation granularity
    return 0;
}
SHIM(kernel32, VirtualAlloc, 16) {
    uint32_t want = arg(c, 0), size = arg(c, 1);
    if (want) return want;  // committing what we already handed out
    return galloc(size, true);
}
SHIM(kernel32, VirtualFree, 12) {
    if (arg(c, 2) & 0x8000) gfree(arg(c, 0));  // MEM_RELEASE
    return 1;
}
SHIM(kernel32, LoadLibraryA, 4) {
    std::string n = gstr(arg(c, 0));
    if (trace_imports) log("  LoadLibraryA %s", n.c_str());
    static std::map<std::string, uint32_t> modules;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    uint32_t &h = modules[n];
    if (!h) h = gstrdup(n);  // the handle is the guest copy of its name
    return h;
}
SHIM(kernel32, FreeLibrary, 4) { return 1; }
SHIM(kernel32, GetProcAddress, 8) {
    std::string dll = arg(c, 0) == 0x400000 ? "daybreak" : gstr(arg(c, 0));
    uint32_t p = arg(c, 1);
    std::string name = p < 0x10000 ? "#" + std::to_string(p) : gstr(p);
    uint32_t a = resolve_import(dll, name);
    if (!a) log("recomp: GetProcAddress(%s, %s): no shim", dll.c_str(), name.c_str());
    return a;
}

// ---- time ----
SHIM(kernel32, GetTickCount, 0) { return uint32_t(now_us() / 1000); }
SHIM(kernel32, QueryPerformanceCounter, 4) {
    st64(arg(c, 0), now_us());
    return 1;
}
SHIM(kernel32, QueryPerformanceFrequency, 4) {
    st64(arg(c, 0), 1000000);
    return 1;
}
SHIM(kernel32, GetSystemTimeAsFileTime, 4) {
    using namespace std::chrono;
    uint64_t t = uint64_t(duration_cast<microseconds>(system_clock::now().time_since_epoch()).count()) * 10 +
                 116444736000000000ull;
    st64(arg(c, 0), t);
    return 0;
}

// ---- text ----
SHIM(kernel32, lstrlenA, 4) { return arg(c, 0) ? uint32_t(strlen(gstr(arg(c, 0)))) : 0; }
SHIM(kernel32, lstrcmpiA, 8) { return uint32_t(strcasecmp(gstr(arg(c, 0)), gstr(arg(c, 1)))); }
SHIM(kernel32, MultiByteToWideChar, 24) {
    uint32_t cp = arg(c, 0), src = arg(c, 2), dst = arg(c, 4), cap = arg(c, 5);
    int32_t len = int32_t(arg(c, 3));
    std::string in = len < 0 ? std::string(gstr(src)) + '\0' : std::string(gstr(src), size_t(len));
    std::vector<uint16_t> out;
    if (cp == 932 || cp == 0 || cp == 3) {
        iconv_t cd = iconv_open("UTF-16LE", "CP932");
        if (cd != iconv_t(-1)) {
            std::vector<char> buf(in.size() * 4 + 8);
            char *ip = &in[0], *op = buf.data();
            size_t il = in.size(), ol = buf.size();
            iconv(cd, &ip, &il, &op, &ol);
            iconv_close(cd);
            size_t n = size_t(op - buf.data()) / 2;
            out.resize(n);
            memcpy(out.data(), buf.data(), n * 2);
        }
    }
    if (out.empty() && !in.empty())
        for (unsigned char ch : in) out.push_back(ch);
    if (!cap) return uint32_t(out.size());
    uint32_t n = std::min<uint32_t>(uint32_t(out.size()), cap);
    for (uint32_t i = 0; i < n; i++) st16(dst + 2 * i, out[i]);
    return n;
}
