#include "movie.h"

#include <algorithm>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include "../third_party/pl_mpeg.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace db {

// One reader: a pl_mpeg decoder fed chunk by chunk from the archive entry.
struct Movie::Stream {
    const Source* src = nullptr;
    const Entry* entry = nullptr;
    uint64_t pos = 0;
    plm_t* plm = nullptr;
    std::vector<uint8_t> chunk;

    ~Stream() {
        if (plm) plm_destroy(plm);
    }

    static void load(plm_buffer_t* buf, void* user) {
        auto* s = static_cast<Stream*>(user);
        constexpr size_t kChunk = 256 * 1024;
        std::string err;
        if (s->pos >= s->entry->size || !s->src->read_range(*s->entry, s->pos, kChunk, s->chunk, &err) ||
            s->chunk.empty()) {
            plm_buffer_signal_end(buf);
            return;
        }
        s->pos += s->chunk.size();
        plm_buffer_write(buf, s->chunk.data(), s->chunk.size());
    }

    bool open(const Source& source, const Entry& e, bool video, std::string* err) {
        src = &source;
        entry = &e;
        plm_buffer_t* buf = plm_buffer_create_with_capacity(1024 * 1024);
        plm_buffer_set_load_callback(buf, &Stream::load, this);
        plm = plm_create_with_buffer(buf, 1);
        if (!plm) {
            if (err) *err = "not an MPEG-1 stream";
            return false;
        }
        plm_set_video_enabled(plm, video ? 1 : 0);
        plm_set_audio_enabled(plm, video ? 0 : 1);
        plm_set_loop(plm, 0);
        if (!plm_has_headers(plm)) {
            if (err) *err = "no MPEG-1 headers in " + e.name;
            return false;
        }
        return true;
    }
};

Movie::Movie() = default;
Movie::~Movie() = default;

bool Movie::open(const Source& src, const std::string& name, std::string* err) {
    video_.reset();
    audio_.reset();
    const Entry* e = src.find(name);
    if (!e) {
        if (err) *err = "no movie " + name;
        return false;
    }
    auto v = std::make_unique<Stream>();
    if (!v->open(src, *e, true, err)) return false;
    if (plm_get_width(v->plm) <= 0 || plm_get_height(v->plm) <= 0) {
        if (err) *err = "no video in " + name;
        return false;
    }
    video_ = std::move(v);
    auto a = std::make_unique<Stream>();
    std::string ignore;
    if (a->open(src, *e, false, &ignore) && plm_get_num_audio_streams(a->plm) > 0) audio_ = std::move(a);
    return true;
}

int Movie::width() const { return video_ ? plm_get_width(video_->plm) : 0; }
int Movie::height() const { return video_ ? plm_get_height(video_->plm) : 0; }
double Movie::fps() const { return video_ ? plm_get_framerate(video_->plm) : 0; }
int Movie::sample_rate() const { return audio_ ? plm_get_samplerate(audio_->plm) : 0; }

bool Movie::next_frame(std::vector<uint8_t>& rgba, double* time) {
    if (!video_) return false;
    plm_frame_t* f = plm_decode_video(video_->plm);
    if (!f) return false;
    rgba.resize(size_t(f->width) * f->height * 4);
    plm_frame_to_rgba(f, rgba.data(), int(f->width) * 4);
    if (time) *time = f->time;
    return true;
}

bool Movie::next_audio(std::vector<int16_t>& stereo) {
    stereo.clear();
    if (!audio_) return false;
    plm_samples_t* s = plm_decode_audio(audio_->plm);
    if (!s) return false;
    stereo.resize(size_t(s->count) * 2);
    for (size_t i = 0; i < stereo.size(); ++i) {
        const float v = std::clamp(s->interleaved[i], -1.f, 1.f);
        stereo[i] = int16_t(v * 32767.f);
    }
    return true;
}

}  // namespace db
