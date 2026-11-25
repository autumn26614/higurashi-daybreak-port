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
    std::atomic<int> facets{0};
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
    // Filters the game added (held, as the graph does), and a renderer of the game's own
    // among them (Kai's, which draws the movie into a texture): the pictures go to its
    // input pin as RGB24 samples, from our output pin, instead of to rt::movie.
    std::vector<uint32_t> filters;
    uint32_t sink_filter = 0, sink_pin = 0, sink_input = 0;  // IBaseFilter, IPin, IMemInputPin
    uint32_t out_pin = 0, allocator = 0, sample = 0, media_type = 0;
    uint32_t sample_buf = 0, sample_bytes = 0;
    int sink_w = 0, sink_h = 0;
    std::atomic<int> sink_state{0};  // the renderer's: 0 stopped, 1 paused, 2 running
    std::vector<uint8_t> bgr;
    std::atomic<bool> player_alive{false}, dead{false}, released{false};

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

// The interfaces of a graph are separate guest objects; when the game has released them
// all, the graph stops (as DirectShow's does when it is destroyed). The player thread is
// left to finish on its own: it may be inside the game's reader. The filters are let go
// of by whichever comes last, this or the player.
void sink_to(CPU *c, Graph &g, int state);
void release_all(CPU *c, Graph &g);
struct Facet : ComObject {
    std::shared_ptr<Graph> g;
    explicit Facet(std::shared_ptr<Graph> graph) : g(std::move(graph)) { g->facets++; }
    ~Facet() override {
        if (--g->facets == 0) {
            g->state = 0;
            g->stop = true;
            sink_to(current(), *g, 0);  // a Receive waiting in the renderer returns
            g->dead = true;
            if (!g->player_alive) release_all(current(), *g);
            if (g->has_player) pthread_detach(g->player);
            g->has_player = false;
        }
    }
};
std::shared_ptr<Graph> graph(CPU *c) {
    auto f = com_as<Facet>(arg(c, 0));
    if (!f) fatal("DirectShow call on unknown object %08x", arg(c, 0));
    return f->g;
}

uint32_t vcall(CPU *c, uint32_t obj, int i, std::initializer_list<uint32_t> args) {
    std::vector<uint32_t> a{obj};
    a.insert(a.end(), args.begin(), args.end());
    return call_guest(c, ld32(ld32(obj) + 4 * uint32_t(i)), a.data(), a.size());
}
void release(CPU *c, uint32_t &obj) {
    if (obj) vcall(c, obj, 2, {});
    obj = 0;
}

// ---- the game's renderer ----
// DirectShow method indices.
enum {
    kFilterStop = 4, kFilterPause = 5, kFilterRun = 6, kFilterEnumPins = 10,  // IBaseFilter
    kEnumNext = 3,                                                           // IEnumPins
    kPinReceiveConnection = 4, kPinDisconnect = 5, kPinQueryDirection = 9,  // IPin
    kMemNotifyAllocator = 4, kMemReceive = 6,                                // IMemInputPin
};

void put_guid(uint32_t at, const char *s) {  // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    unsigned d1, d2, d3, b[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
           &b[6], &b[7]);
    st32(at, d1);
    st16(at + 4, uint16_t(d2));
    st16(at + 6, uint16_t(d3));
    for (int i = 0; i < 8; i++) st8(at + 8 + uint32_t(i), uint8_t(b[i]));
}

// The unconnected input pin of a filter the game added, if any (AddRef'd).
uint32_t find_input_pin(CPU *c, uint32_t filter) {
    uint32_t out = galloc(12, true);
    uint32_t found = 0;
    if (vcall(c, filter, kFilterEnumPins, {out}) == S_OK && ld32(out)) {
        uint32_t e = ld32(out);
        while (!found && vcall(c, e, kEnumNext, {1, out + 4, out + 8}) == S_OK && ld32(out + 4)) {
            uint32_t pin = ld32(out + 4);
            if (vcall(c, pin, kPinQueryDirection, {out + 8}) == S_OK && ld32(out + 8) == 0)  // PINDIR_INPUT
                found = pin;
            else
                vcall(c, pin, 2, {});
        }
        vcall(c, e, 2, {});
    }
    gfree(out);
    return found;
}

// The media type our output pin offers: bottom-up RGB24 (as a DirectShow decoder gives).
uint32_t make_media_type(int w, int h, uint32_t bytes) {
    uint32_t mt = galloc(72 + 88, true);
    uint32_t vih = mt + 72;
    put_guid(mt, "73646976-0000-0010-8000-00AA00389B71");       // MEDIATYPE_Video
    put_guid(mt + 16, "E436EB7D-524F-11CE-9F53-0020AF0BA770");  // MEDIASUBTYPE_RGB24
    st32(mt + 32, 1);                                           // bFixedSizeSamples
    st32(mt + 40, bytes);                                       // lSampleSize
    put_guid(mt + 44, "05589F80-C356-11CE-BF01-00AA0055595A");  // FORMAT_VideoInfo
    st32(mt + 64, 88);                                          // cbFormat
    st32(mt + 68, vih);
    st32(vih + 8, uint32_t(w));  // rcSource, rcTarget
    st32(vih + 12, uint32_t(h));
    st32(vih + 24, uint32_t(w));
    st32(vih + 28, uint32_t(h));
    uint32_t bmi = vih + 48;  // BITMAPINFOHEADER
    st32(bmi, 40);
    st32(bmi + 4, uint32_t(w));
    st32(bmi + 8, uint32_t(h));
    st16(bmi + 12, 1);
    st16(bmi + 14, 24);
    st32(bmi + 20, bytes);
    return mt;
}

struct HostPin : ComObject {};
struct Allocator : ComObject {};
struct Sample : ComObject {
    uint32_t buf = 0, size = 0;
};

uint32_t e_notimpl(CPU *) { return E_NOTIMPL; }
uint32_t s_ok(CPU *) { return S_OK; }
uint32_t s_false(CPU *) { return S_FALSE; }

const Method out_pin_vt[] = {
    COM_IUNKNOWN,
    {"Connect", 3, e_notimpl}, {"ReceiveConnection", 3, e_notimpl}, {"Disconnect", 1, s_ok},
    {"ConnectedTo", 2, e_notimpl}, {"ConnectionMediaType", 2, e_notimpl},
    {"QueryPinInfo", 2, [](CPU *c) {
         uint32_t info = arg(c, 1);
         st32(info, 0);
         st32(info + 4, 1);  // PINDIR_OUTPUT
         st16(info + 8, 'O');
         st16(info + 10, 0);
         return S_OK;
     }},
    {"QueryDirection", 2, [](CPU *c) {
         st32(arg(c, 1), 1);  // PINDIR_OUTPUT
         return S_OK;
     }},
    {"QueryId", 2, e_notimpl}, {"QueryAccept", 2, s_ok}, {"EnumMediaTypes", 2, e_notimpl},
    {"QueryInternalConnections", 3, e_notimpl}, {"EndOfStream", 1, s_ok}, {"BeginFlush", 1, s_ok},
    {"EndFlush", 1, s_ok}, {"NewSegment", 7, s_ok},
};

// One buffer: the renderer draws each sample before Receive returns (it has no clock).
const Method allocator_vt[] = {
    COM_IUNKNOWN,
    {"SetProperties", 3, [](CPU *c) {
         for (int i = 0; i < 16; i += 4) st32(arg(c, 2) + uint32_t(i), ld32(arg(c, 1) + uint32_t(i)));
         return S_OK;
     }},
    {"GetProperties", 2, e_notimpl}, {"Commit", 1, s_ok}, {"Decommit", 1, s_ok},
    {"GetBuffer", 5, e_notimpl}, {"ReleaseBuffer", 2, s_ok},
};

const Method sample_vt[] = {
    {"QueryInterface", 3, [](CPU *c) {
         if (guid_string(arg(c, 1)) == "{36B73884-C2C8-11CF-8B46-00805F6CEF60}") {  // IMediaSample2:
             st32(arg(c, 2), 0);  // the renderer asks for every sample, and copes without
             return 0x80004002u;
         }
         return com_query_interface(c);
     }},
    {"AddRef", 1, com_add_ref}, {"Release", 1, com_release},
    {"GetPointer", 2, [](CPU *c) {
         st32(arg(c, 1), com_this<Sample>(c)->buf);
         return S_OK;
     }},
    {"GetSize", 1, [](CPU *c) { return com_this<Sample>(c)->size; }},
    {"GetTime", 3, [](CPU *) { return 0x80040249u; }},  // VFW_E_SAMPLE_TIME_NOT_SET: draw it now
    {"SetTime", 3, s_ok}, {"IsSyncPoint", 1, s_ok}, {"SetSyncPoint", 2, s_ok}, {"IsPreroll", 1, s_false},
    {"SetPreroll", 2, s_ok}, {"GetActualDataLength", 1, [](CPU *c) { return com_this<Sample>(c)->size; }},
    {"SetActualDataLength", 2, s_ok},
    {"GetMediaType", 2, [](CPU *c) {
         st32(arg(c, 1), 0);
         return S_FALSE;
     }},
    {"SetMediaType", 2, s_ok}, {"IsDiscontinuity", 1, s_false}, {"SetDiscontinuity", 2, s_ok},
    {"GetMediaTime", 3, [](CPU *) { return 0x80040251u; }},  // VFW_E_MEDIA_TIME_NOT_SET
    {"SetMediaTime", 3, s_ok},
};

// Connects our output pin to the input pin of a renderer the game added, if there is one.
void connect_sink(CPU *c, Graph &g, int w, int h) {
    if (g.sink_pin || w <= 0 || h <= 0) return;
    for (uint32_t f : g.filters) {
        uint32_t pin = find_input_pin(c, f);
        if (!pin) continue;
        uint32_t pitch = (uint32_t(w) * 3 + 3) & ~3u;
        uint32_t bytes = pitch * uint32_t(h);
        auto op = std::make_shared<HostPin>();
        op->iids = {"{56A86891-0AD4-11CE-B03A-0020AF0BA770}"};  // IPin
        g.out_pin = com_new(op, com_vtable("IPin(out)", out_pin_vt));
        g.media_type = make_media_type(w, h, bytes);
        uint32_t hr = vcall(c, pin, kPinReceiveConnection, {g.out_pin, g.media_type});
        if (hr != S_OK) {
            log("recomp: DirectShow: the game's renderer refused %dx%d RGB24 (%08x)", w, h, hr);
            vcall(c, pin, 2, {});
            continue;
        }
        uint32_t out = galloc(4, true), iid = galloc(16);
        put_guid(iid, "56A8689D-0AD4-11CE-B03A-0020AF0BA770");  // IID_IMemInputPin
        vcall(c, pin, 0, {iid, out});
        g.sink_input = ld32(out);
        gfree(iid);
        gfree(out);
        if (!g.sink_input) {
            vcall(c, pin, kPinDisconnect, {});
            vcall(c, pin, 2, {});
            continue;
        }
        auto a = std::make_shared<Allocator>();
        a->iids = {"{56A8689C-0AD4-11CE-B03A-0020AF0BA770}"};  // IMemAllocator
        g.allocator = com_new(a, com_vtable("IMemAllocator", allocator_vt));
        auto s = std::make_shared<Sample>();
        s->iids = {"{56A8689A-0AD4-11CE-B03A-0020AF0BA770}"};  // IMediaSample
        s->buf = g.sample_buf = galloc(bytes, true);
        s->size = g.sample_bytes = bytes;
        g.sample = com_new(s, com_vtable("IMediaSample", sample_vt));
        vcall(c, g.sink_input, kMemNotifyAllocator, {g.allocator, 0});
        g.sink_filter = f;
        g.sink_pin = pin;
        g.sink_w = w;
        g.sink_h = h;
        log("recomp: DirectShow: movie %dx%d goes to the game's renderer", w, h);
        return;
    }
}

void sink_to(CPU *c, Graph &g, int state) {
    if (!g.sink_filter || !c || g.sink_state == state) return;
    if (state == 0) {
        vcall(c, g.sink_filter, kFilterStop, {});
    } else {
        if (g.sink_state == 0 || state == 1) vcall(c, g.sink_filter, kFilterPause, {});
        if (state == 2) vcall(c, g.sink_filter, kFilterRun, {0, 0});
    }
    g.sink_state = state;
}

// Once the game has released the graph and the player is gone: lets go of the filters.
void release_all(CPU *c, Graph &g) {
    if (!c || g.released.exchange(true)) return;
    sink_to(c, g, 0);
    if (g.sink_pin) vcall(c, g.sink_pin, kPinDisconnect, {});
    release(c, g.sink_input);
    release(c, g.sink_pin);
    for (uint32_t &f : g.filters) release(c, f);
    g.filters.clear();
    g.sink_filter = 0;
}

// A decoded picture to the game's renderer: bottom-up BGR rows, as a DIB.
void deliver(Graph &g, plm_frame_t *f) {
    if (!g.cpu || g.sink_state != 2) return;
    int w = std::min(int(f->width), g.sink_w), h = std::min(int(f->height), g.sink_h);
    g.bgr.resize(size_t(f->width) * f->height * 3);
    plm_frame_to_bgr(f, g.bgr.data(), int(f->width) * 3);
    uint32_t pitch = (uint32_t(g.sink_w) * 3 + 3) & ~3u;
    for (int y = 0; y < h; y++)
        memcpy(GM + g.sample_buf + uint32_t(g.sink_h - 1 - y) * pitch, g.bgr.data() + size_t(y) * f->width * 3,
               size_t(w) * 3);
    vcall(g.cpu, g.sink_input, kMemReceive, {g.sample});
}

// ---- playback ----
void on_video(plm_t *, plm_frame_t *f, void *user) {
    Graph *g = static_cast<Graph *>(user);
    if (g->sink_input) return deliver(*g, f);
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
        // RECOMP_SKIP_MOVIES (headless tests): a second of the movie, without sound, then the
        // end (the game's reader keeps to the movie's bitrate, so faster decoding cannot help).
        static const bool fast = getenv("RECOMP_SKIP_MOVIES") && *getenv("RECOMP_SKIP_MOVIES");
        if (fast) {
            plm_set_audio_enabled(g->plm, 0);
            if (g->position > 1.0) {
                g->state = 0;
                g->post(1, S_OK, 0);  // EC_COMPLETE
                break;
            }
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
    g->player_alive = false;
    if (g->dead) release_all(c, *g);
    g->cpu = nullptr;
    free_cpu(c);
}

// Starts the player thread (paused or running), or carries on with the one there is.
uint32_t start(CPU *c, std::shared_ptr<Graph> g, int state) {
    if (!g->reader && g->stream.empty()) return S_FALSE;
    sink_to(c, *g, state);
    if (g->has_player && g->player_alive) {
        g->state = state;
        return S_OK;
    }
    g->halt();  // one that has reached the end
    g->state = state;
    {
        std::lock_guard<std::mutex> lock(current_mutex);
        playing = g;
    }
    g->player_alive = true;
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
         if (!f) return 0x80004003u;  // E_POINTER
         vcall(c, f, 1, {});  // the graph holds a reference
         graph(c)->filters.push_back(f);
         return S_OK;
     }},
    {"RemoveFilter", 2, ok}, {"EnumFilters", 2, not_impl},
    {"FindFilterByName", 3, [](CPU *) { return VFW_E_NOT_FOUND; }},
    {"ConnectDirect", 4, ok}, {"Reconnect", 2, ok}, {"Disconnect", 2, ok}, {"SetDefaultSyncSource", 1, ok},
    {"Connect", 3, ok},
    {"Render", 2, [](CPU *c) {
         auto g = graph(c);
         if (!open_reader(c, *g, arg(c, 1))) return VFW_E_CANNOT_RENDER;
         if (g->filters.size() > 1) {
             // A renderer of the game's own may be among them: it needs the picture size
             // now, to make its texture, so the stream headers are read here.
             g->cpu = c;
             int w = open_decoder(*g) ? plm_get_width(g->plm) : 0;
             int h = g->plm ? plm_get_height(g->plm) : 0;
             g->cpu = nullptr;
             connect_sink(c, *g, w, h);
         }
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
    {"Run", 1, [](CPU *c) { return start(c, graph(c), 2); }},
    {"Pause", 1, [](CPU *c) {
         start(c, graph(c), 1);
         return S_OK;
     }},
    {"Stop", 1, [](CPU *c) {
         auto g = graph(c);
         g->state = 0;
         sink_to(c, *g, 0);  // first: the player may be waiting in the renderer
         g->halt();
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
        auto f = std::make_shared<Facet>(g);
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
    if (!g || g->state == 0 || !g->visible || g->sink_input) return false;
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
