// user32, gdi32, winmm, imm32, ole32, oleaut32: one window, a message queue, timers, mmio.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "host.h"
#include "runtime.h"
#include "win.h"

using namespace rt;

namespace rt {
WindowState window;

namespace {
std::mutex queue_mutex;
std::condition_variable queue_cv;
std::deque<Msg> queue;
}  // namespace

void post_message(const Msg &m) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queue.push_back(m);
    queue_cv.notify_all();
}

static bool take_message(Msg &m, bool remove) {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (queue.empty()) return false;
    m = queue.front();
    if (remove) queue.pop_front();
    return true;
}

static void write_msg(uint32_t a, const Msg &m) {
    st32(a, m.hwnd);
    st32(a + 4, m.message);
    st32(a + 8, m.wparam);
    st32(a + 12, m.lparam);
    st32(a + 16, uint32_t(now_us() / 1000));
    st32(a + 20, 0);
    st32(a + 24, 0);
}

uint32_t send_message(CPU *c, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    if (!window.wndproc) return 0;
    return call_guest(c, window.wndproc, {window.hwnd, msg, wparam, lparam});
}
}  // namespace rt

// ---- user32 ----
SHIM(user32, RegisterClassExA, 4) {
    window.wndproc = ld32(arg(c, 0) + 8);
    return 0xC001;
}
SHIM(user32, CreateWindowExA, 48) {
    window.hwnd = 0x00010010;
    window.width = arg(c, 8) ? arg(c, 8) : 800;
    window.height = arg(c, 9) ? arg(c, 9) : 600;
    if (trace_imports) log("  CreateWindowExA %s %ux%u", gstr(arg(c, 2)) ? gstr(arg(c, 2)) : "", arg(c, 8), arg(c, 9));
    uint32_t cs = galloc(48, true);
    st32(cs, arg(c, 11));  // lpCreateParams
    st32(cs + 4, 0x400000);
    st32(cs + 20, arg(c, 9));
    st32(cs + 24, arg(c, 8));
    send_message(c, 0x0001, 0, cs);  // WM_CREATE
    gfree(cs);
    return window.hwnd;
}
SHIM(user32, DestroyWindow, 4) {
    send_message(c, 0x0002, 0, 0);  // WM_DESTROY
    return 1;
}
SHIM(user32, ShowWindow, 8) { return 0; }
SHIM(user32, UpdateWindow, 4) { return 1; }
SHIM(user32, SetActiveWindow, 4) { return window.hwnd; }
SHIM(user32, SetWindowPos, 28) { return 1; }
SHIM(user32, SetWindowLongA, 12) { return 0; }
SHIM(user32, SetClassLongA, 12) { return 0; }
SHIM(user32, LoadIconA, 8) { return 0x20001; }
SHIM(user32, LoadCursorA, 8) { return 0x20002; }
SHIM(user32, ShowCursor, 4) { return 0; }
SHIM(user32, GetSystemMetrics, 4) {
    switch (arg(c, 0)) {
    case 0: return 1024;  // SM_CXSCREEN
    case 1: return 768;   // SM_CYSCREEN
    case 4: return 19;    // SM_CYCAPTION
    case 5: case 6: return 1;
    case 7: case 8: return 3;  // SM_CXFIXEDFRAME
    case 32: case 33: return 4;
    default: return 0;
    }
}
SHIM(user32, SetRect, 20) {
    uint32_t r = arg(c, 0);
    for (int i = 0; i < 4; i++) st32(r + 4 * i, arg(c, 1 + i));
    return 1;
}
SHIM(user32, GetClientRect, 8) {
    uint32_t r = arg(c, 1);
    st32(r, 0);
    st32(r + 4, 0);
    st32(r + 8, window.width);
    st32(r + 12, window.height);
    return 1;
}
SHIM(user32, GetWindowInfo, 8) {
    uint32_t o = arg(c, 1);
    memset(GM + o + 4, 0, 56);
    st32(o, 60);
    st32(o + 12, window.width);
    st32(o + 16, window.height);
    st32(o + 28, window.width);
    st32(o + 32, window.height);
    return 1;
}
SHIM(user32, GetDC, 4) { return 0x30001; }
SHIM(user32, ReleaseDC, 8) { return 1; }
SHIM(user32, MessageBoxA, 16) {
    log("recomp: MessageBox [%s] %s", gstr(arg(c, 2)) ? gstr(arg(c, 2)) : "", gstr(arg(c, 1)) ? gstr(arg(c, 1)) : "");
    return 1;  // IDOK
}
SHIM(user32, DefWindowProcA, 16) { return 0; }
SHIM(user32, PostQuitMessage, 4) {
    post_message({0, 0x0012, arg(c, 0), 0});  // WM_QUIT
    return 0;
}
SHIM(user32, RegisterWindowMessageA, 4) {
    static std::atomic<uint32_t> next{0xC100};
    return next++;
}
SHIM(user32, PostThreadMessageA, 16) {
    post_message({0, arg(c, 1), arg(c, 2), arg(c, 3)});
    return 1;
}
SHIM(user32, GetQueueStatus, 4) {
    Msg m;
    return take_message(m, false) ? 0x00080008 : 0;
}
SHIM(user32, PeekMessageA, 20) {
    if (window.on_idle) window.on_idle(c);
    Msg m;
    if (!take_message(m, (arg(c, 4) & 1) != 0)) return 0;
    write_msg(arg(c, 0), m);
    return 1;
}
SHIM(user32, GetMessageA, 16) {
    Msg m;
    while (!take_message(m, true)) {
        if (window.on_idle) window.on_idle(c);
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait_for(lock, std::chrono::milliseconds(5));
    }
    write_msg(arg(c, 0), m);
    return m.message == 0x0012 ? 0 : 1;
}
SHIM(user32, TranslateMessage, 4) { return 0; }
SHIM(user32, DispatchMessageA, 4) {
    uint32_t m = arg(c, 0);
    return send_message(c, ld32(m + 4), ld32(m + 8), ld32(m + 12));
}
SHIM(user32, MsgWaitForMultipleObjects, 20) {
    uint32_t n = arg(c, 0), list = arg(c, 1), ms = arg(c, 3);
    auto deadline = now_us() + uint64_t(ms) * 1000;
    for (;;) {
        for (uint32_t i = 0; i < n; i++) {
            auto o = handle_get(ld32(list + 4 * i));
            if (o && o->wait(0)) return i;
        }
        Msg m;
        if (take_message(m, false)) return n;
        if (ms != 0xFFFFFFFF && now_us() >= deadline) return 0x102;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
SHIM(user32, MapVirtualKeyA, 8) { return arg(c, 0); }

// ---- gdi32 (text is drawn by the font module; these keep handles) ----
SHIM(gdi32, GetStockObject, 4) { return 0x40000 + arg(c, 0); }
SHIM(gdi32, SelectObject, 8) { return 0x40000; }
SHIM(gdi32, DeleteObject, 4) { return 1; }

// ---- winmm ----
SHIM(winmm, timeBeginPeriod, 4) { return 0; }
SHIM(winmm, timeGetTime, 0) { return uint32_t(now_us() / 1000); }
SHIM(winmm, waveOutGetVolume, 8) {
    if (arg(c, 1)) st32(arg(c, 1), 0xFFFFFFFF);
    return 0;
}
SHIM(winmm, waveOutSetVolume, 8) { return 0; }
SHIM(winmm, timeSetEvent, 20) {
    uint32_t delay = arg(c, 0), cb = arg(c, 2), user = arg(c, 3), flags = arg(c, 4);
    static std::atomic<uint32_t> next_id{1};
    uint32_t id = next_id++;
    std::thread([=] {
        CPU *tc = new_cpu(kThreadStack);
        auto next = std::chrono::steady_clock::now();
        do {
            next += std::chrono::milliseconds(delay ? delay : 1);
            std::this_thread::sleep_until(next);
            call_guest(tc, cb, {id, 0, user, 0, 0});
        } while (flags & 1);  // TIME_PERIODIC
        free_cpu(tc);
    }).detach();
    return id;
}

// mmio: RIFF files, on disk or in a memory buffer (FOURCC_MEM).
namespace {
struct Mmio : KObject {
    std::vector<uint8_t> data;
    uint32_t pos = 0;
};
uint32_t fourcc(const char *s) { return uint32_t(s[0]) | uint32_t(s[1]) << 8 | uint32_t(s[2]) << 16 | uint32_t(s[3]) << 24; }
}  // namespace
SHIM(winmm, mmioOpenA, 12) {
    auto m = std::make_shared<Mmio>();
    uint32_t info = arg(c, 1);
    if (info && ld32(info + 4) == fourcc("MEM ")) {
        uint32_t buf = ld32(info + 16), n = ld32(info + 12);
        m->data.assign(GM + buf, GM + buf + n);
    } else {
        std::string path = host_path(gstr(arg(c, 0)));
        FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
        if (!f) return 0;
        fseek(f, 0, SEEK_END);
        m->data.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(m->data.data(), 1, m->data.size(), f) != m->data.size()) m->data.clear();
        fclose(f);
    }
    return handle_new(m);
}
SHIM(winmm, mmioClose, 8) {
    handle_close(arg(c, 0));
    return 0;
}
SHIM(winmm, mmioRead, 12) {
    auto m = handle_as<Mmio>(arg(c, 0));
    if (!m) return 0xFFFFFFFF;
    uint32_t n = std::min<uint32_t>(arg(c, 2), uint32_t(m->data.size()) - std::min<uint32_t>(m->pos, uint32_t(m->data.size())));
    memcpy(GM + arg(c, 1), m->data.data() + m->pos, n);
    m->pos += n;
    return n;
}
SHIM(winmm, mmioDescend, 16) {
    auto m = handle_as<Mmio>(arg(c, 0));
    uint32_t ck = arg(c, 1), parent = arg(c, 2), flags = arg(c, 3);
    if (!m) return 1;
    uint32_t end = parent ? ld32(parent + 12) + ld32(parent + 4) : uint32_t(m->data.size());
    uint32_t want_id = ld32(ck), want_type = ld32(ck + 8);
    auto rd = [&](uint32_t o) { uint32_t v; memcpy(&v, m->data.data() + o, 4); return v; };
    while (m->pos + 8 <= end && m->pos + 8 <= m->data.size()) {
        uint32_t id = rd(m->pos), size = rd(m->pos + 4);
        uint32_t data = m->pos + 8;
        bool list = id == fourcc("RIFF") || id == fourcc("LIST");
        uint32_t type = list && data + 4 <= m->data.size() ? rd(data) : 0;
        bool match = true;
        if (flags & 0x20) match = id == fourcc("RIFF") && type == want_type;       // MMIO_FINDRIFF
        else if (flags & 0x40) match = id == fourcc("LIST") && type == want_type;  // MMIO_FINDLIST
        else if (flags & 0x10) match = id == want_id;                              // MMIO_FINDCHUNK
        if (match) {
            st32(ck, id);
            st32(ck + 4, size);
            st32(ck + 8, type);
            st32(ck + 12, data);
            st32(ck + 16, 0);
            m->pos = list ? data + 4 : data;
            return 0;
        }
        m->pos = data + size + (size & 1);
    }
    return 0x103;  // MMIOERR_CHUNKNOTFOUND
}
SHIM(winmm, mmioAscend, 12) {
    auto m = handle_as<Mmio>(arg(c, 0));
    if (!m) return 1;
    uint32_t ck = arg(c, 1);
    uint32_t size = ld32(ck + 4);
    m->pos = ld32(ck + 12) + size + (size & 1);
    return 0;
}

// ---- imm32 (no input method) ----
SHIM(imm32, ImmGetContext, 4) { return 0; }
SHIM(imm32, ImmReleaseContext, 8) { return 1; }
SHIM(imm32, ImmGetOpenStatus, 4) { return 0; }
SHIM(imm32, ImmGetCompositionStringA, 16) { return 0; }
SHIM(imm32, ImmGetDefaultIMEWnd, 4) { return 0; }

// ---- ole32 / oleaut32 ----
SHIM(ole32, CoInitialize, 4) { return 0; }
SHIM(ole32, CoUninitialize, 0) { return 0; }
SHIM(ole32, CoFreeUnusedLibraries, 0) { return 0; }
SHIM(ole32, CoTaskMemAlloc, 4) { return galloc(arg(c, 0)); }
SHIM(ole32, CoTaskMemFree, 4) {
    gfree(arg(c, 0));
    return 0;
}
SHIM(ole32, CoCreateInstance, 20) {
    std::string clsid = guid_string(arg(c, 0)), iid = guid_string(arg(c, 3));
    uint32_t out = arg(c, 4);
    uint32_t obj = com_create(c, clsid, iid);
    if (trace_imports || !obj) log("  CoCreateInstance %s %s -> %08x", clsid.c_str(), iid.c_str(), obj);
    if (out) st32(out, obj);
    return obj ? 0 : 0x80040154;  // REGDB_E_CLASSNOTREG
}
SHIM_AS(oleaut32, SysAllocString, "#2", 4) {
    uint32_t s = arg(c, 0);
    if (!s) return 0;
    uint32_t n = 0;
    while (ld16(s + 2 * n)) n++;
    uint32_t b = galloc(4 + 2 * n + 2);
    st32(b, 2 * n);
    memcpy(GM + b + 4, GM + s, 2 * n + 2);
    return b + 4;
}
SHIM_AS(oleaut32, SysFreeString, "#6", 4) {
    if (arg(c, 0)) gfree(arg(c, 0) - 4);
    return 0;
}
