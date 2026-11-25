// DirectSound 8 over a software mixer. Buffer memory is guest memory (the game locks it and
// writes PCM, streaming music into ring buffers); the mixer reads it as it plays.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "audio.h"
#include "host.h"
#include "win.h"

using namespace rt;

void rt_set_event(uint32_t h);  // shims_kernel32.cpp

namespace {
constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct SoundBuffer;
std::recursive_mutex mixer_mutex;
std::vector<SoundBuffer *> playing;  // buffers that are part of the mix

struct SoundData {  // shared by a buffer and its duplicates
    uint32_t mem = 0, bytes = 0;
    ~SoundData() { gfree(mem); }
};

struct SoundBuffer : ComObject {
    std::shared_ptr<SoundData> data;
    uint32_t channels = 2, rate = 22050, bits = 16, block = 4;
    uint32_t flags = 0;
    bool primary = false;
    // Playback
    bool is_playing = false, looping = false;
    double pos = 0;  // in frames
    int32_t volume = 0, pan = 0;
    uint32_t frequency = 0;
    std::vector<std::pair<uint32_t, uint32_t>> notify;  // offset, event handle
    // 3D
    bool has3d = false;
    Vec3 position;
    float min_distance = 1, max_distance = 1e9f;
    uint32_t mode3d = 0;  // DS3DMODE_NORMAL
    ~SoundBuffer() override {
        std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
        playing.erase(std::remove(playing.begin(), playing.end(), this), playing.end());
    }
};

struct Listener {
    Vec3 position, front{0, 0, 1}, top{0, 1, 0};
    float rolloff = 1, distance_factor = 1;
} listener;

void signal_notify(SoundBuffer &b, uint32_t from, uint32_t to, bool wrapped) {
    for (auto &n : b.notify) {
        if (n.first == 0xFFFFFFFF) continue;
        bool hit = wrapped ? (n.first >= from || n.first < to) : (n.first >= from && n.first < to);
        if (hit) rt_set_event(n.second);
    }
}

float gain_of(const SoundBuffer &b, float &left, float &right) {
    float g = b.volume <= -10000 ? 0.0f : std::pow(10.0f, float(b.volume) / 2000.0f);
    float p = float(b.pan) / 10000.0f;
    left = g * (p > 0 ? std::pow(10.0f, -p * 5.0f / 2.0f) : 1.0f);
    right = g * (p < 0 ? std::pow(10.0f, p * 5.0f / 2.0f) : 1.0f);
    if (b.has3d && b.mode3d != 2) {  // not DS3DMODE_DISABLE
        Vec3 d{b.position.x, b.position.y, b.position.z};
        if (b.mode3d == 0) {
            d.x -= listener.position.x;
            d.y -= listener.position.y;
            d.z -= listener.position.z;
        }
        float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) * listener.distance_factor;
        float att = dist <= b.min_distance ? 1.0f : b.min_distance / (b.min_distance + listener.rolloff * (std::min(dist, b.max_distance) - b.min_distance));
        // Right vector = top x front.
        Vec3 r{listener.top.y * listener.front.z - listener.top.z * listener.front.y,
               listener.top.z * listener.front.x - listener.top.x * listener.front.z,
               listener.top.x * listener.front.y - listener.top.y * listener.front.x};
        float rl = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        float side = dist > 1e-3f && rl > 1e-6f ? (d.x * r.x + d.y * r.y + d.z * r.z) / (dist / listener.distance_factor * rl) : 0;
        left *= att * std::min(1.0f, 1.0f - side * 0.5f);
        right *= att * std::min(1.0f, 1.0f + side * 0.5f);
    }
    return g;
}
}  // namespace

namespace rt {
namespace audio {

namespace {
std::mutex stream_mutex;
std::deque<float> stream;  // interleaved stereo at kRate
}  // namespace

void stream_push(const float *in, int frames, int rate, float gain) {
    std::lock_guard<std::mutex> lock(stream_mutex);
    double step = double(rate) / kRate;
    int outn = int(frames / step);
    for (int i = 0; i < outn; i++) {
        int k = std::min(frames - 1, int(i * step));
        stream.push_back(in[2 * k] * gain);
        stream.push_back(in[2 * k + 1] * gain);
    }
}

double stream_queued() {
    std::lock_guard<std::mutex> lock(stream_mutex);
    return double(stream.size() / 2) / kRate;
}

void mix(int16_t *out, int frames) {
    std::vector<float> acc(size_t(frames) * 2, 0.0f);
    {
        std::lock_guard<std::mutex> lock(stream_mutex);
        size_t n = std::min(acc.size(), stream.size());
        for (size_t i = 0; i < n; i++) acc[i] += stream[i];
        stream.erase(stream.begin(), stream.begin() + long(n));
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
        for (size_t k = 0; k < playing.size();) {
            SoundBuffer &b = *playing[k];
            if (!b.is_playing || !b.data || !b.data->bytes) {
                playing.erase(playing.begin() + long(k));
                continue;
            }
            float gl, gr;
            gain_of(b, gl, gr);
            uint32_t total = b.data->bytes / b.block;
            double step = double(b.frequency ? b.frequency : b.rate) / kRate;
            uint32_t start_byte = uint32_t(b.pos) * b.block;
            bool wrapped = false, ended = false;
            const uint8_t *mem = GM + b.data->mem;
            for (int i = 0; i < frames; i++) {
                uint32_t f = uint32_t(b.pos);
                if (f >= total) {
                    if (b.looping) {
                        b.pos -= total;
                        f = uint32_t(b.pos);
                        wrapped = true;
                    } else {
                        ended = true;
                        break;
                    }
                }
                const uint8_t *s = mem + size_t(f) * b.block;
                float l, r;
                if (b.bits == 16) {
                    int16_t a, c2;
                    memcpy(&a, s, 2);
                    if (b.channels > 1) memcpy(&c2, s + 2, 2); else c2 = a;
                    l = a / 32768.0f;
                    r = c2 / 32768.0f;
                } else {
                    l = (s[0] - 128) / 128.0f;
                    r = b.channels > 1 ? (s[1] - 128) / 128.0f : l;
                }
                acc[size_t(i) * 2] += l * gl;
                acc[size_t(i) * 2 + 1] += r * gr;
                b.pos += step;
            }
            uint32_t end_byte = std::min<uint32_t>(uint32_t(b.pos), total) * b.block;
            if (!b.notify.empty()) signal_notify(b, start_byte, end_byte, wrapped);
            if (ended) {
                b.is_playing = false;
                b.pos = 0;
                for (auto &n : b.notify)
                    if (n.first == 0xFFFFFFFF) rt_set_event(n.second);
                playing.erase(playing.begin() + long(k));
                continue;
            }
            k++;
        }
    }
    for (size_t i = 0; i < acc.size(); i++) {
        float v = std::max(-1.0f, std::min(1.0f, acc[i]));
        out[i] = int16_t(v * 32767.0f);
    }
}

void start_headless() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        std::vector<int16_t> buf(2 * 441);
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
            mix(buf.data(), 441);
        }
    }).detach();
}

}  // namespace audio
}  // namespace rt

namespace {
// IDirectSound3DBuffer / 3DListener / Notify are separate guest objects (facets) that
// forward to their buffer.
struct Facet : ComObject {
    std::shared_ptr<SoundBuffer> owner;
};
std::shared_ptr<SoundBuffer> buf(CPU *c) {
    uint32_t self = arg(c, 0);
    if (auto f = com_as<Facet>(self)) return f->owner;
    if (auto b = com_as<SoundBuffer>(self)) return b;
    fatal("DirectSound call on unknown object %08x", self);
}

uint32_t lock_buffer(CPU *c) {
    auto b = buf(c);
    if (!b->data) return DSERR_INVALIDPARAM;
    uint32_t size = b->data->bytes, off = arg(c, 1), n = arg(c, 2), flags = arg(c, 7);
    if (flags & 1) off = uint32_t(b->pos) * b->block % size;  // DSBLOCK_FROMWRITECURSOR
    if (flags & 2) n = size;                                   // DSBLOCK_ENTIREBUFFER
    off %= size;
    n = std::min(n, size);
    uint32_t first = std::min(n, size - off);
    st32(arg(c, 3), b->data->mem + off);
    st32(arg(c, 4), first);
    if (arg(c, 5)) st32(arg(c, 5), n > first ? b->data->mem : 0);
    if (arg(c, 6)) st32(arg(c, 6), n - first);
    return DS_OK;
}

const Method buffer3d_vt[] = {
    COM_IUNKNOWN,
    {"GetAllParameters", 2, nullptr}, {"GetConeAngles", 3, nullptr}, {"GetConeOrientation", 2, nullptr},
    {"GetConeOutsideVolume", 2, nullptr},
    {"GetMaxDistance", 2, [](CPU *c) { stf32(arg(c, 1), buf(c)->max_distance); return DS_OK; }},
    {"GetMinDistance", 2, [](CPU *c) { stf32(arg(c, 1), buf(c)->min_distance); return DS_OK; }},
    {"GetMode", 2, [](CPU *c) { st32(arg(c, 1), buf(c)->mode3d); return DS_OK; }},
    {"GetPosition", 2, [](CPU *c) {
         auto b = buf(c);
         stf32(arg(c, 1), b->position.x);
         stf32(arg(c, 1) + 4, b->position.y);
         stf32(arg(c, 1) + 8, b->position.z);
         return DS_OK;
     }},
    {"GetVelocity", 2, nullptr},
    {"SetAllParameters", 3, [](CPU *c) {
         auto b = buf(c);
         uint32_t p = arg(c, 1);
         b->position = {float(ldf32(p + 4)), float(ldf32(p + 8)), float(ldf32(p + 12))};
         b->min_distance = float(ldf32(p + 56));
         b->max_distance = float(ldf32(p + 60));
         b->mode3d = ld32(p + 64);
         return DS_OK;
     }},
    {"SetConeAngles", 4, [](CPU *) { return DS_OK; }},
    {"SetConeOrientation", 5, [](CPU *) { return DS_OK; }},
    {"SetConeOutsideVolume", 3, [](CPU *) { return DS_OK; }},
    {"SetMaxDistance", 3, [](CPU *c) { buf(c)->max_distance = float(ldf32(c->r[4] + 8)); return DS_OK; }},
    {"SetMinDistance", 3, [](CPU *c) { buf(c)->min_distance = float(ldf32(c->r[4] + 8)); return DS_OK; }},
    {"SetMode", 3, [](CPU *c) { buf(c)->mode3d = arg(c, 1); return DS_OK; }},
    {"SetPosition", 5, [](CPU *c) {
         buf(c)->position = {float(ldf32(c->r[4] + 8)), float(ldf32(c->r[4] + 12)), float(ldf32(c->r[4] + 16))};
         return DS_OK;
     }},
    {"SetVelocity", 5, [](CPU *) { return DS_OK; }},
};

const Method listener_vt[] = {
    COM_IUNKNOWN,
    {"GetAllParameters", 2, nullptr}, {"GetDistanceFactor", 2, nullptr}, {"GetDopplerFactor", 2, nullptr},
    {"GetOrientation", 3, nullptr}, {"GetPosition", 2, nullptr}, {"GetRolloffFactor", 2, nullptr},
    {"GetVelocity", 2, nullptr},
    {"SetAllParameters", 3, [](CPU *) { return DS_OK; }},
    {"SetDistanceFactor", 3, [](CPU *c) { listener.distance_factor = float(ldf32(c->r[4] + 8)); return DS_OK; }},
    {"SetDopplerFactor", 3, [](CPU *) { return DS_OK; }},
    {"SetOrientation", 8, [](CPU *c) {
         uint32_t p = c->r[4] + 8;
         listener.front = {float(ldf32(p)), float(ldf32(p + 4)), float(ldf32(p + 8))};
         listener.top = {float(ldf32(p + 12)), float(ldf32(p + 16)), float(ldf32(p + 20))};
         return DS_OK;
     }},
    {"SetPosition", 5, [](CPU *c) {
         listener.position = {float(ldf32(c->r[4] + 8)), float(ldf32(c->r[4] + 12)), float(ldf32(c->r[4] + 16))};
         return DS_OK;
     }},
    {"SetRolloffFactor", 3, [](CPU *c) { listener.rolloff = float(ldf32(c->r[4] + 8)); return DS_OK; }},
    {"SetVelocity", 5, [](CPU *) { return DS_OK; }},
    {"CommitDeferredSettings", 1, [](CPU *) { return DS_OK; }},
};

const Method notify_vt[] = {
    COM_IUNKNOWN,
    {"SetNotificationPositions", 3, [](CPU *c) {
         auto b = buf(c);
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         b->notify.clear();
         for (uint32_t i = 0; i < arg(c, 1); i++)
             b->notify.push_back({ld32(arg(c, 2) + 8 * i), ld32(arg(c, 2) + 8 * i + 4)});
         return DS_OK;
     }},
};

// The other interfaces of a buffer are separate guest objects pointing at the same host
// buffer (QueryInterface hands them out).
uint32_t buffer_query(CPU *c);

const Method buffer_vt[] = {
    {"QueryInterface", 3, buffer_query}, {"AddRef", 1, com_add_ref}, {"Release", 1, com_release},
    {"GetCaps", 2, [](CPU *c) {
         auto b = buf(c);
         uint32_t p = arg(c, 1);
         st32(p + 4, b->flags);
         st32(p + 8, b->data ? b->data->bytes : 0);
         st32(p + 12, 0);
         st32(p + 16, 0);
         return DS_OK;
     }},
    {"GetCurrentPosition", 3, [](CPU *c) {
         auto b = buf(c);
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         uint32_t size = b->data && b->data->bytes ? b->data->bytes : 1;
         uint32_t play = (uint32_t(b->pos) * b->block) % size;
         uint32_t ahead = b->rate / 100 * b->block;  // the write cursor runs ~10 ms ahead
         if (arg(c, 1)) st32(arg(c, 1), play);
         if (arg(c, 2)) st32(arg(c, 2), b->is_playing ? (play + ahead) % size : play);
         return DS_OK;
     }},
    {"GetFormat", 4, [](CPU *c) {
         auto b = buf(c);
         uint32_t p = arg(c, 1);
         if (p && arg(c, 2) >= 16) {
             st16(p, 1);
             st16(p + 2, b->channels);
             st32(p + 4, b->rate);
             st32(p + 8, b->rate * b->block);
             st16(p + 12, b->block);
             st16(p + 14, b->bits);
             if (arg(c, 2) >= 18) st16(p + 16, 0);
         }
         if (arg(c, 3)) st32(arg(c, 3), 18);
         return DS_OK;
     }},
    {"GetVolume", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(buf(c)->volume)); return DS_OK; }},
    {"GetPan", 2, [](CPU *c) { st32(arg(c, 1), uint32_t(buf(c)->pan)); return DS_OK; }},
    {"GetFrequency", 2, [](CPU *c) {
         auto b = buf(c);
         st32(arg(c, 1), b->frequency ? b->frequency : b->rate);
         return DS_OK;
     }},
    {"GetStatus", 2, [](CPU *c) {
         auto b = buf(c);
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         st32(arg(c, 1), b->is_playing ? (1u | (b->looping ? 4u : 0u)) : 0u);
         return DS_OK;
     }},
    {"Initialize", 3, [](CPU *) { return DS_OK; }},
    {"Lock", 8, lock_buffer},
    {"Play", 4, [](CPU *c) {
         auto b = buf(c);
         if (b->primary) return DS_OK;
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         b->looping = (arg(c, 3) & 1) != 0;
         if (!b->is_playing) {
             b->is_playing = true;
             playing.push_back(b.get());
         }
         return DS_OK;
     }},
    {"SetCurrentPosition", 2, [](CPU *c) {
         auto b = buf(c);
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         b->pos = double(arg(c, 1) / b->block);
         return DS_OK;
     }},
    {"SetFormat", 2, [](CPU *) { return DS_OK; }},
    {"SetVolume", 2, [](CPU *c) { buf(c)->volume = int32_t(arg(c, 1)); return DS_OK; }},
    {"SetPan", 2, [](CPU *c) { buf(c)->pan = int32_t(arg(c, 1)); return DS_OK; }},
    {"SetFrequency", 2, [](CPU *c) { buf(c)->frequency = arg(c, 1); return DS_OK; }},
    {"Stop", 1, [](CPU *c) {
         auto b = buf(c);
         std::lock_guard<std::recursive_mutex> lock(mixer_mutex);
         b->is_playing = false;
         playing.erase(std::remove(playing.begin(), playing.end(), b.get()), playing.end());
         return DS_OK;
     }},
    {"Unlock", 5, [](CPU *) { return DS_OK; }},
    {"Restore", 1, [](CPU *) { return DS_OK; }},
    {"SetFX", 4, nullptr}, {"AcquireResources", 4, nullptr}, {"GetObjectInPath", 5, nullptr},
};

uint32_t buffer_query(CPU *c) {
    auto b = com_as<SoundBuffer>(arg(c, 0));
    std::string iid = guid_string(arg(c, 1));
    const Method *vt = nullptr;
    size_t n = 0;
    std::string name;
    if (iid == "{279AFA86-4981-11CE-A521-0020AF0BE560}") {  // IDirectSound3DBuffer
        vt = buffer3d_vt, n = sizeof buffer3d_vt / sizeof *buffer3d_vt, name = "IDirectSound3DBuffer8";
    } else if (iid == "{279AFA84-4981-11CE-A521-0020AF0BE560}") {  // IDirectSound3DListener
        vt = listener_vt, n = sizeof listener_vt / sizeof *listener_vt, name = "IDirectSound3DListener8";
    } else if (iid == "{B0210783-89CD-11D0-AF08-00A0C925CD16}") {  // IDirectSoundNotify
        vt = notify_vt, n = sizeof notify_vt / sizeof *notify_vt, name = "IDirectSoundNotify";
    }
    if (b && vt) {
        auto f = std::make_shared<Facet>();
        f->owner = b;
        f->iids = {iid};
        st32(arg(c, 2), com_new(f, com_vtable(name, vt, n)));
        return DS_OK;
    }
    return com_query_interface(c);
}

}  // namespace

namespace {
struct DSound : ComObject {};

uint32_t create_buffer(CPU *c) {
    uint32_t desc = arg(c, 1);
    uint32_t flags = ld32(desc + 4), bytes = ld32(desc + 8), wfx = ld32(desc + 16);
    auto b = std::make_shared<SoundBuffer>();
    b->flags = flags;
    b->primary = (flags & 1) != 0;
    b->has3d = (flags & 0x10) != 0;  // DSBCAPS_CTRL3D
    if (wfx) {
        b->channels = ld16(wfx + 2);
        b->rate = ld32(wfx + 4);
        b->bits = ld16(wfx + 14);
        b->block = ld16(wfx + 12) ? ld16(wfx + 12) : b->channels * b->bits / 8;
    }
    if (!b->primary) {
        b->data = std::make_shared<SoundData>();
        b->data->bytes = bytes;
        b->data->mem = galloc(bytes ? bytes : 4, true);
    }
    b->iids = {"{279AFA85-4981-11CE-A521-0020AF0BE560}", "{6825A449-7524-4D82-920F-50E36AB3AB1E}"};
    st32(arg(c, 2), com_new(b, com_vtable("IDirectSoundBuffer8", buffer_vt)));
    return DS_OK;
}

const Method dsound_vt[] = {
    COM_IUNKNOWN,
    {"CreateSoundBuffer", 4, create_buffer},
    {"GetCaps", 2, [](CPU *c) {
         uint32_t p = arg(c, 1);
         uint32_t size = ld32(p);
         memset(GM + p + 4, 0, size - 4);
         st32(p + 4, 0xF5F);  // continuous rates, 8/16-bit, mono/stereo, primary and secondary
         st32(p + 8, 100);
         st32(p + 12, 100000);
         return DS_OK;
     }},
    {"DuplicateSoundBuffer", 3, [](CPU *c) {
         auto src = com_as<SoundBuffer>(arg(c, 1));
         if (!src) return DSERR_INVALIDPARAM;
         auto b = std::make_shared<SoundBuffer>();
         b->data = src->data;
         b->channels = src->channels;
         b->rate = src->rate;
         b->bits = src->bits;
         b->block = src->block;
         b->flags = src->flags;
         b->has3d = src->has3d;
         b->volume = src->volume;
         b->pan = src->pan;
         b->frequency = src->frequency;
         b->iids = src->iids;
         st32(arg(c, 2), com_new(b, com_vtable("IDirectSoundBuffer8", buffer_vt)));
         return DS_OK;
     }},
    {"SetCooperativeLevel", 3, [](CPU *) { return DS_OK; }},
    {"Compact", 1, [](CPU *) { return DS_OK; }},
    {"GetSpeakerConfig", 2, [](CPU *c) { st32(arg(c, 1), 4); return DS_OK; }},  // stereo
    {"SetSpeakerConfig", 2, [](CPU *) { return DS_OK; }},
    {"Initialize", 2, [](CPU *) { return DS_OK; }},
    {"VerifyCertification", 2, [](CPU *c) { st32(arg(c, 1), 0); return DS_OK; }},
};

ComClassReg dsound_class("{3901CC3F-84B5-4FA4-BA35-AA8172B8A09B}", [](CPU *, const std::string &) {
    auto o = std::make_shared<DSound>();
    o->iids = {"{C50A7E93-F395-4834-9EF6-7FA99DE50966}", "{279AFA83-4981-11CE-A521-0020AF0BE560}"};
    return com_new(o, com_vtable("IDirectSound8", dsound_vt));
});
}  // namespace
