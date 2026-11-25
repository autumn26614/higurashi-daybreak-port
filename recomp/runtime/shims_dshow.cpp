// DirectShow for the game's movies: a filter graph that reads the MPEG-1 stream through the
// game's own source filter (an IAsyncReader over its archives), decodes it with pl_mpeg in
// real time, sends the sound to the mixer and the pictures to rt::movie, and reports
// EC_COMPLETE at the end. IVideoWindow places the picture, as the game's child window would.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#define PL_MPEG_IMPLEMENTATION
#include "../../port/third_party/pl_mpeg.h"

#include "audio.h"
#include "host.h"
#include "movie.h"
#include "win.h"

using namespace rt;

uint32_t rt_create_event(bool manual, bool initial);  // shims_kernel32.cpp
void rt_set_event(uint32_t h);
void rt_reset_event(uint32_t h);

namespace {
constexpr uint32_t S_OK = 0, S_FALSE = 1, E_NOTIMPL = 0x80004001, E_ABORT = 0x80004004,
                   VFW_E_NOT_FOUND = 0x80040216, VFW_E_CANNOT_RENDER = 0x80040218;

struct Graph {
    std::mutex m;
    // The stream: through the game's IAsyncReader (pulled as the decoder needs it, on the
    // player thread, as DirectShow's own threads would), or a file read whole.
    uint32_t reader = 0;
    uint64_t length = 0, read_pos = 0;
    uint32_t read_buf = 0;
    CPU *cpu = nullptr;  // the player thread's guest CPU, for the reads
    std::vector<uint8_t> stream;
    plm_t *plm = nullptr;
    pthread_t player{};
    bool has_player = false;
    std::atomic<bool> stop{false};
    std::atomic<int> state{0};  // 0 stopped, 1 paused, 2 running
    double position = 0;
    // events
    std::vector<std::array<uint32_t, 3>> events;
    uint32_t event_handle = 0;
    uint32_t notify_hwnd = 0, notify_msg = 0, notify_data = 0;
    // video window
    int x = 0, y = 0, w = 0, h = 0;
    bool visible = true;
    int32_t volume = 0;
    // the picture
    movie::Frame frame;
    uint64_t serial = 0;

    ~Graph() {
        halt();
        if (plm) plm_destroy(plm);
    }
    void halt() {
        stop = true;
        if (has_player) pthread_join(player, nullptr);
        has_player = false;
        stop = false;
    }
    void post(uint32_t code, uint32_t p1, uint32_t p2) {
        {
            std::lock_guard<std::mutex> lock(m);
            events.push_back({code, p1, p2});
        }
        if (event_handle) rt_set_event(event_handle);
        if (notify_hwnd) post_message({notify_hwnd, notify_msg, 0, notify_data});
    }
};

std::mutex current_mutex;
std::weak_ptr<Graph> playing;

struct Facet : ComObject {
    std::shared_ptr<Graph> g;
};
std::shared_ptr<Graph> graph(CPU *c) {
    auto f = com_as<Facet>(arg(c, 0));
    if (!f) fatal("DirectShow call on unknown object %08x", arg(c, 0));
    return f->g;
}

uint32_t vcall(CPU *c, uint32_t obj, int i, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    uint32_t fn = ld32(ld32(obj) + 4 * uint32_t(i));
    switch (a.size()) {
    case 1: return call_guest(c, fn, {a[0]});
    case 3: return call_guest(c, fn, {a[0], a[1], a[2]});
    case 5: return call_guest(c, fn, {a[0], a[1], a[2], a[3], a[4]});
    default: fatal("vcall arity");
    }
}

// ---- playback ----
void on_video(plm_t *, plm_frame_t *f, void *user) {
    Graph *g = static_cast<Graph *>(user);
    std::vector<uint8_t> rgba(size_t(f->width) * f->height * 4);
    plm_frame_to_rgba(f, rgba.data(), int(f->width) * 4);
    std::lock_guard<std::mutex> lock(g->m);
    g->frame.width = int(f->width);
    g->frame.height = int(f->height);
    g->frame.rgba.swap(rgba);
    g->frame.serial = ++g->serial;
}

void on_audio(plm_t *plm, plm_samples_t *s, void *user) {
    Graph *g = static_cast<Graph *>(user);
    float gain = g->volume <= -10000 ? 0.0f : std::pow(10.0f, float(g->volume) / 2000.0f);
    audio::stream_push(s->interleaved, int(s->count), plm_get_samplerate(plm), gain);
}

constexpr uint32_t kChunk = 64 * 1024;

void load_more(plm_buffer_t *buf, void *user) {
    Graph *g = static_cast<Graph *>(user);
    if (g->read_pos >= g->length || !g->cpu) {
        plm_buffer_signal_end(buf);
        return;
    }
    uint32_t len = uint32_t(std::min<uint64_t>(kChunk, g->length - g->read_pos));
    uint32_t hr = vcall(g->cpu, g->reader, 7, {uint32_t(g->read_pos), uint32_t(g->read_pos >> 32), len, g->read_buf});
    if (hr != S_OK && hr != S_FALSE) {
        plm_buffer_signal_end(buf);
        return;
    }
    plm_buffer_write(buf, GM + g->read_buf, len);
    g->read_pos += len;
}

bool open_decoder(Graph &g) {
    if (g.plm) return true;
    if (g.reader) {
        plm_buffer_t *buf = plm_buffer_create_with_capacity(4 * kChunk);
        plm_buffer_set_load_callback(buf, load_more, &g);
        g.plm = plm_create_with_buffer(buf, 1);
    } else if (!g.stream.empty()) {
        g.plm = plm_create_with_memory(g.stream.data(), g.stream.size(), 0);
    }
    if (!g.plm) return false;
    plm_set_video_decode_callback(g.plm, on_video, &g);
    plm_set_audio_decode_callback(g.plm, on_audio, &g);
    plm_set_audio_enabled(g.plm, 1);
    log("recomp: movie %dx%d, %llu bytes", plm_get_width(g.plm), plm_get_height(g.plm),
        (unsigned long long)(g.reader ? g.length : g.stream.size()));
    return true;
}

void run_player(std::shared_ptr<Graph> g) {
    g->cpu = new_cpu(kThreadStack);
    if (!open_decoder(*g)) {
        g->state = 0;
        g->post(1, S_OK, 0);
    }
    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    while (!g->stop && g->plm) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto now = clock::now();
        double dt = std::min(0.1, std::chrono::duration<double>(now - last).count());
        last = now;
        if (g->state != 2) continue;
        // RECOMP_SKIP_MOVIES (headless tests): play 200 times as fast, without sound.
        static const bool fast = getenv("RECOMP_SKIP_MOVIES") && *getenv("RECOMP_SKIP_MOVIES");
        if (fast) {
            plm_set_audio_enabled(g->plm, 0);
            dt *= 200;
        } else if (audio::stream_queued() > 0.25) {
            continue;  // keep pace with the sound actually played
        }
        plm_decode(g->plm, dt);
        g->position += dt;
        if (plm_has_ended(g->plm)) {
            g->state = 0;
            g->post(1, S_OK, 0);  // EC_COMPLETE
            break;
        }
    }
    CPU *c = g->cpu;
    g->cpu = nullptr;
    free_cpu(c);
}

uint32_t start(CPU *, std::shared_ptr<Graph> g) {
    if (!g->reader && g->stream.empty()) return S_FALSE;
    if (g->state == 2 && g->has_player) return S_OK;
    g->halt();
    g->state = 2;
    {
        std::lock_guard<std::mutex> lock(current_mutex);
        playing = g;
    }
    g->player = spawn_thread([g] { run_player(g); }, false);
    g->has_player = true;
    return S_OK;
}

// Finds the source pin's IAsyncReader and the stream length.
bool open_reader(CPU *c, Graph &g, uint32_t pin) {
    uint32_t out = galloc(4, true);
    uint32_t iid = galloc(16);
    // IID_IAsyncReader {56A868AA-0AD4-11CE-B03A-0020AF0BA770}
    const uint8_t guid[16] = {0xAA, 0x68, 0xA8, 0x56, 0xD4, 0x0A, 0xCE, 0x11, 0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70};
    memcpy(GM + iid, guid, 16);
    bool ok = false;
    if (vcall(c, pin, 0, {iid, out}) == 0 && ld32(out)) {
        g.reader = ld32(out);
        uint32_t total = galloc(16, true);
        vcall(c, g.reader, 8, {total, total + 8});  // Length(&total, &available)
        g.length = ld64(total);
        gfree(total);
        g.read_pos = 0;
        if (!g.read_buf) g.read_buf = galloc(kChunk);
        ok = g.length > 0;
    }
    gfree(iid);
    gfree(out);
    return ok;
}

// ---- interfaces ----
uint32_t not_impl(CPU *) { return E_NOTIMPL; }
uint32_t ok(CPU *) { return S_OK; }
#define DISPATCH {"GetTypeInfoCount", 2, not_impl}, {"GetTypeInfo", 4, not_impl}, {"GetIDsOfNames", 6, not_impl}, \
    {"Invoke", 9, not_impl}

uint32_t graph_query(CPU *c);
#define UNKNOWN {"QueryInterface", 3, graph_query}, {"AddRef", 1, com_add_ref}, {"Release", 1, com_release}

const Method builder_vt[] = {
    UNKNOWN,
    {"AddFilter", 3, [](CPU *c) {
         uint32_t f = arg(c, 1);
         if (f) call_guest(c, ld32(ld32(f) + 4), {f});  // the graph holds a reference
         return S_OK;
     }},
    {"RemoveFilter", 2, ok}, {"EnumFilters", 2, not_impl},
    {"FindFilterByName", 3, [](CPU *) { return VFW_E_NOT_FOUND; }},
    {"ConnectDirect", 4, ok}, {"Reconnect", 2, ok}, {"Disconnect", 2, ok}, {"SetDefaultSyncSource", 1, ok},
    {"Connect", 3, ok},
    {"Render", 2, [](CPU *c) {
         auto g = graph(c);
         if (!open_reader(c, *g, arg(c, 1))) return VFW_E_CANNOT_RENDER;
         return S_OK;
     }},
    {"RenderFile", 3, [](CPU *c) {
         auto g = graph(c);
         std::string name;
         for (uint32_t a = arg(c, 1); ld16(a); a += 2) name += char(ld16(a));
         std::string path = host_path(name);
         FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
         if (!f) return VFW_E_NOT_FOUND;
         fseek(f, 0, SEEK_END);
         g->stream.resize(size_t(ftell(f)));
         fseek(f, 0, SEEK_SET);
         size_t got = fread(g->stream.data(), 1, g->stream.size(), f);
         fclose(f);
         return got == g->stream.size() ? S_OK : VFW_E_CANNOT_RENDER;
     }},
    {"AddSourceFilter", 4, not_impl}, {"SetLogFile", 2, ok}, {"Abort", 1, ok}, {"ShouldOperationContinue", 1, ok},
};

const Method control_vt[] = {
    UNKNOWN, DISPATCH,
    {"Run", 1, [](CPU *c) { return start(c, graph(c)); }},
    {"Pause", 1, [](CPU *c) {
         auto g = graph(c);
         if (!g->has_player) start(c, g);
         g->state = 1;
         return S_OK;
     }},
    {"Stop", 1, [](CPU *c) {
         auto g = graph(c);
         g->halt();
         g->state = 0;
         return S_OK;
     }},
    {"GetState", 3, [](CPU *c) {
         if (arg(c, 2)) st32(arg(c, 2), uint32_t(graph(c)->state));
         return S_OK;
     }},
    {"RenderFile", 2, not_impl}, {"AddSourceFilter", 3, not_impl}, {"get_FilterCollection", 2, not_impl},
    {"get_RegFilterCollection", 2, not_impl}, {"StopWhenReady", 1, ok},
};

uint32_t get_event(CPU *c) {
    auto g = graph(c);
    std::lock_guard<std::mutex> lock(g->m);
    if (g->events.empty()) {
        if (g->event_handle) rt_reset_event(g->event_handle);
        return E_ABORT;
    }
    auto e = g->events.front();
    g->events.erase(g->events.begin());
    st32(arg(c, 1), e[0]);
    st32(arg(c, 2), e[1]);
    st32(arg(c, 3), e[2]);
    if (g->events.empty() && g->event_handle) rt_reset_event(g->event_handle);
    return S_OK;
}

const Method event_vt[] = {
    UNKNOWN, DISPATCH,
    {"GetEventHandle", 2, [](CPU *c) {
         auto g = graph(c);
         if (!g->event_handle) g->event_handle = rt_create_event(true, false);
         st32(arg(c, 1), g->event_handle);
         return S_OK;
     }},
    {"GetEvent", 5, get_event},
    {"WaitForCompletion", 3, [](CPU *c) {
         auto g = graph(c);
         uint32_t ms = arg(c, 1);
         auto deadline = now_us() + uint64_t(ms) * 1000;
         while (g->state == 2 && (ms == 0xFFFFFFFF || now_us() < deadline))
             std::this_thread::sleep_for(std::chrono::milliseconds(5));
         if (arg(c, 2)) st32(arg(c, 2), g->state == 2 ? 0 : 1);
         return g->state == 2 ? E_ABORT : S_OK;
     }},
    {"CancelDefaultHandling", 2, ok}, {"RestoreDefaultHandling", 2, ok}, {"FreeEventParams", 4, ok},
    {"SetNotifyWindow", 4, [](CPU *c) {
         auto g = graph(c);
         g->notify_hwnd = arg(c, 1);
         g->notify_msg = arg(c, 2);
         g->notify_data = arg(c, 3);
         return S_OK;
     }},
    {"SetNotifyFlags", 2, ok},
    {"GetNotifyFlags", 2, [](CPU *c) {
         st32(arg(c, 1), 0);
         return S_OK;
     }},
};

uint32_t put_zero(CPU *c) {
    if (arg(c, 1)) st32(arg(c, 1), 0);
    return S_OK;
}
const Method window_vt[] = {
    UNKNOWN, DISPATCH,
    {"put_Caption", 2, ok}, {"get_Caption", 2, not_impl}, {"put_WindowStyle", 2, ok}, {"get_WindowStyle", 2, put_zero},
    {"put_WindowStyleEx", 2, ok}, {"get_WindowStyleEx", 2, put_zero}, {"put_AutoShow", 2, ok},
    {"get_AutoShow", 2, put_zero}, {"put_WindowState", 2, ok}, {"get_WindowState", 2, put_zero},
    {"put_BackgroundPalette", 2, ok}, {"get_BackgroundPalette", 2, put_zero},
    {"put_Visible", 2, [](CPU *c) {
         graph(c)->visible = arg(c, 1) != 0;
         return S_OK;
     }},
    {"get_Visible", 2, [](CPU *c) {
         st32(arg(c, 1), graph(c)->visible ? 0xFFFFFFFF : 0);
         return S_OK;
     }},
    {"put_Left", 2, [](CPU *c) { graph(c)->x = int(arg(c, 1)); return S_OK; }},
    {"get_Left", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(graph(c)->x)); return S_OK; }},
    {"put_Width", 2, [](CPU *c) { graph(c)->w = int(arg(c, 1)); return S_OK; }},
    {"get_Width", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(graph(c)->w)); return S_OK; }},
    {"put_Top", 2, [](CPU *c) { graph(c)->y = int(arg(c, 1)); return S_OK; }},
    {"get_Top", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(graph(c)->y)); return S_OK; }},
    {"put_Height", 2, [](CPU *c) { graph(c)->h = int(arg(c, 1)); return S_OK; }},
    {"get_Height", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(graph(c)->h)); return S_OK; }},
    {"put_Owner", 2, ok}, {"get_Owner", 2, put_zero}, {"put_MessageDrain", 2, ok}, {"get_MessageDrain", 2, put_zero},
    {"get_BorderColor", 2, put_zero}, {"put_BorderColor", 2, ok}, {"get_FullScreenMode", 2, put_zero},
    {"put_FullScreenMode", 2, ok}, {"SetWindowForeground", 2, ok}, {"NotifyOwnerMessage", 5, ok},
    {"SetWindowPosition", 5, [](CPU *c) {
         auto g = graph(c);
         g->x = int(arg(c, 1));
         g->y = int(arg(c, 2));
         g->w = int(arg(c, 3));
         g->h = int(arg(c, 4));
         return S_OK;
     }},
    {"GetWindowPosition", 5, [](CPU *c) {
         auto g = graph(c);
         st32(arg(c, 1), uint32_t(g->x));
         st32(arg(c, 2), uint32_t(g->y));
         st32(arg(c, 3), uint32_t(g->w));
         st32(arg(c, 4), uint32_t(g->h));
         return S_OK;
     }},
    {"GetMinIdealImageSize", 3, not_impl}, {"GetMaxIdealImageSize", 3, not_impl},
    {"GetRestoredWindowPosition", 5, not_impl}, {"HideCursor", 2, ok}, {"IsCursorHidden", 2, put_zero},
};

const Method audio_vt[] = {
    UNKNOWN, DISPATCH,
    {"put_Volume", 2, [](CPU *c) { graph(c)->volume = int32_t(arg(c, 1)); return S_OK; }},
    {"get_Volume", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(graph(c)->volume)); return S_OK; }},
    {"put_Balance", 2, ok}, {"get_Balance", 2, put_zero},
};

const Method position_vt[] = {
    UNKNOWN, DISPATCH,
    {"get_Duration", 2, [](CPU *c) {
         auto g = graph(c);
         stf64(arg(c, 1), g->plm ? plm_get_duration(g->plm) : 0.0);
         return S_OK;
     }},
    {"put_CurrentPosition", 3, [](CPU *c) {
         auto g = graph(c);
         double t = ldf64(c->r[4] + 8);
         if (g->plm && t == 0 && g->state != 2 && !g->reader) plm_rewind(g->plm), g->position = 0;
         return S_OK;
     }},
    {"get_CurrentPosition", 2, [](CPU *c) { stf64(arg(c, 1), graph(c)->position); return S_OK; }},
    {"get_StopTime", 2, [](CPU *c) {
         auto g = graph(c);
         stf64(arg(c, 1), g->plm ? plm_get_duration(g->plm) : 0.0);
         return S_OK;
     }},
    {"put_StopTime", 3, ok}, {"get_PrerollTime", 2, [](CPU *c) { stf64(arg(c, 1), 0.0); return S_OK; }},
    {"put_PrerollTime", 3, ok}, {"put_Rate", 3, ok},
    {"get_Rate", 2, [](CPU *c) { stf64(arg(c, 1), 1.0); return S_OK; }},
    {"CanSeekForward", 2, put_zero}, {"CanSeekBackward", 2, put_zero},
};

struct Iface {
    const char *iid, *name;
    const Method *vt;
    size_t n;
};
#define IFACE(iid, name, vt) {iid, name, vt, sizeof vt / sizeof *vt}
const Iface kIfaces[] = {
    IFACE("{56A8689F-0AD4-11CE-B03A-0020AF0BA770}", "IFilterGraph", builder_vt),
    IFACE("{56A868A9-0AD4-11CE-B03A-0020AF0BA770}", "IGraphBuilder", builder_vt),
    IFACE("{36B73882-C2C8-11CF-8B46-00805F6CEF60}", "IFilterGraph2", builder_vt),
    IFACE("{56A868B1-0AD4-11CE-B03A-0020AF0BA770}", "IMediaControl", control_vt),
    IFACE("{56A868B6-0AD4-11CE-B03A-0020AF0BA770}", "IMediaEvent", event_vt),
    IFACE("{56A868C0-0AD4-11CE-B03A-0020AF0BA770}", "IMediaEventEx", event_vt),
    IFACE("{56A868B4-0AD4-11CE-B03A-0020AF0BA770}", "IVideoWindow", window_vt),
    IFACE("{56A868B3-0AD4-11CE-B03A-0020AF0BA770}", "IBasicAudio", audio_vt),
    IFACE("{56A868B2-0AD4-11CE-B03A-0020AF0BA770}", "IMediaPosition", position_vt),
};

uint32_t facet_for(const std::shared_ptr<Graph> &g, const std::string &iid) {
    for (const Iface &i : kIfaces) {
        if (iid != i.iid) continue;
        auto f = std::make_shared<Facet>();
        f->g = g;
        f->iids = {iid};
        return com_new(f, com_vtable(i.name, i.vt, i.n));
    }
    return 0;
}

uint32_t graph_query(CPU *c) {
    auto f = com_as<Facet>(arg(c, 0));
    std::string iid = guid_string(arg(c, 1));
    uint32_t o = f ? facet_for(f->g, iid) : 0;
    if (!o) {
        if (iid == "{00000000-0000-0000-C000-000000000046}") return com_query_interface(c);
        log("recomp: DirectShow QueryInterface %s: not provided", iid.c_str());
        st32(arg(c, 2), 0);
        return 0x80004002;
    }
    st32(arg(c, 2), o);
    return S_OK;
}

ComClassReg filter_graph("{E436EBB3-524F-11CE-9F53-0020AF0BA770}", [](CPU *, const std::string &iid) {
    return facet_for(std::make_shared<Graph>(), iid);
});
}  // namespace

namespace rt {
namespace movie {
bool current(Frame &out) {
    std::shared_ptr<Graph> g;
    {
        std::lock_guard<std::mutex> lock(current_mutex);
        g = playing.lock();
    }
    if (!g || g->state == 0 || !g->visible) return false;
    std::lock_guard<std::mutex> lock(g->m);
    if (g->frame.rgba.empty()) return false;
    if (out.serial != g->frame.serial) {
        out.width = g->frame.width;
        out.height = g->frame.height;
        out.rgba = g->frame.rgba;
        out.serial = g->frame.serial;
    }
    out.x = g->x;
    out.y = g->y;
    out.w = g->w ? g->w : g->frame.width;
    out.h = g->h ? g->h : g->frame.height;
    return true;
}
}  // namespace movie
}  // namespace rt
