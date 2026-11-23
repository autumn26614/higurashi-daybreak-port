#include "audio.h"

#include <cstdlib>
#include <cstring>

#include "cnv.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../third_party/stb_vorbis.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace db {

namespace {

bool ends_with(const std::string& s, const char* suf) {
    const size_t n = strlen(suf);
    return s.size() >= n && lower_ascii(s.substr(s.size() - n)) == suf;
}

bool read_entry(const Source& a, const std::string& name, std::vector<uint8_t>& d, std::string* err) {
    const Entry* e = a.find(name);
    if (!e) {
        if (err) *err = "missing: " + name;
        return false;
    }
    return a.read(*e, d, err);
}

}  // namespace

bool parse_sfl(const std::vector<uint8_t>& d, int64_t& start, int64_t& length) {
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) != 0 || memcmp(d.data() + 8, "SFPL", 4) != 0) return false;
    start = length = -1;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t size = load_le32(&d[p + 4]);
        const uint8_t* body = &d[p + 8];
        if (p + 8 + size > d.size()) break;
        if (memcmp(&d[p], "cue ", 4) == 0 && size >= 4 + 24 && load_le32(body) >= 1)
            start = load_le32(body + 4 + 20);  // first cue point: dwSampleOffset
        if (memcmp(&d[p], "LIST", 4) == 0 && size >= 4 && memcmp(body, "adtl", 4) == 0) {
            for (size_t q = 4; q + 8 <= size;) {
                const uint32_t s2 = load_le32(body + q + 4);
                if (memcmp(body + q, "ltxt", 4) == 0 && s2 >= 8) length = load_le32(body + q + 8 + 4);
                q += 8 + s2 + (s2 & 1);
            }
        }
        p += 8 + size + (size & 1);
    }
    return start >= 0;
}

bool load_sound(const Source& a, const std::string& name, Pcm& out, std::string* err) {
    out = Pcm{};
    std::vector<uint8_t> d;
    if (ends_with(name, ".ogg")) {
        if (!read_entry(a, name, d, err)) return false;
        int channels = 0, rate = 0;
        short* samples = nullptr;
        const int frames = stb_vorbis_decode_memory(d.data(), int(d.size()), &channels, &rate, &samples);
        if (frames < 0 || !samples || channels <= 0) {
            free(samples);
            if (err) *err = "cannot decode " + name;
            return false;
        }
        out.channels = channels;
        out.rate = rate;
        out.samples.assign(samples, samples + size_t(frames) * size_t(channels));
        free(samples);
        std::vector<uint8_t> sfl;
        int64_t start = -1, length = -1;
        const std::string sfl_name = name.substr(0, name.size() - 4) + ".sfl";
        if (a.find(sfl_name) && read_entry(a, sfl_name, sfl, nullptr) && parse_sfl(sfl, start, length) && start < frames) {
            out.loop_start = start;
            out.loop_end = length > 0 ? std::min<int64_t>(start + length, frames) : frames;
        }
        return true;
    }
    const std::string cnv = ends_with(name, ".cnv") ? name : cnv_name(name);
    if (!read_entry(a, cnv, d, err)) return false;
    Sound s;
    if (!decode_sound(d, s, err)) return false;
    out.channels = s.channels;
    out.rate = int(s.rate);
    if (s.bits == 16) {
        out.samples.resize(s.pcm.size() / 2);
        for (size_t i = 0; i < out.samples.size(); i++)
            out.samples[i] = int16_t(uint16_t(s.pcm[i * 2] | (s.pcm[i * 2 + 1] << 8)));
    } else if (s.bits == 8) {
        out.samples.resize(s.pcm.size());
        for (size_t i = 0; i < s.pcm.size(); i++) out.samples[i] = int16_t((int(s.pcm[i]) - 128) << 8);
    } else {
        if (err) *err = "unsupported sample size " + std::to_string(s.bits);
        return false;
    }
    return out.channels > 0 && out.rate > 0;
}

std::string voice_entry(const Source& a, const std::string& voice) {
    if (voice.size() < 5) return "";
    const std::string base = voice.substr(0, voice.size() - 4);  // drop ".wav"
    const std::string candidates[] = {
        "data/voice/WIN/" + base + ".cnv",
        "data/voice/scenario/" + base.substr(0, 3) + "/" + base + ".cnv",
        "data/voice/" + base + ".cnv",
    };
    for (const auto& c : candidates)
        if (a.find(c)) return c;
    return "";
}

}  // namespace db
