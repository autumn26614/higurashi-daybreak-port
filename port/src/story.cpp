#include "story.h"

#include <algorithm>
#include <cstdlib>

namespace db {
namespace story {

namespace {

bool starts(const std::string& s, const char* w, std::string* rest) {
    const std::string word(w);
    if (s.compare(0, word.size(), word) != 0) return false;
    size_t i = word.size();
    while (i < s.size() && s[i] == ' ') i++;
    *rest = s.substr(i);
    return true;
}

std::vector<std::string> split_commas(const std::string& s) {
    std::vector<std::string> out(1);
    for (char c : s) {
        if (c == ',') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

int to_int(const std::string& s) { return std::atoi(s.c_str()); }  // the game uses atoi too

}  // namespace

Story parse(const std::string& all) {
    Story st;
    const std::string bytes = all.substr(0, all.find('\0'));
    std::string chardir, voicedir;
    Scene scene;
    std::string pending_voice;
    int pending_sleep = 0;
    size_t i = 0;
    while (i <= bytes.size()) {
        size_t end = bytes.find("\r\n", i);
        if (end == std::string::npos) end = bytes.size();
        std::string line = bytes.substr(i, end - i);
        i = end + 2;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string rest;
        if (line.empty() || line[0] == '#') continue;
        if (starts(line, "characterdir ", &rest)) {
            chardir = rest, st.has_commands = true;
        } else if (starts(line, "voicedir ", &rest)) {
            voicedir = rest, st.has_commands = true;
        } else if (starts(line, "bgm ", &rest)) {
            scene.bgm = "data/bgm/" + rest, st.has_commands = true;
        } else if (starts(line, "bg ", &rest)) {
            scene.background = rest, st.has_commands = true;
        } else if (starts(line, "character ", &rest)) {
            st.has_commands = true;
            const auto f = split_commas(rest);
            const int slot = f.size() > 1 ? to_int(f[1]) : 0;
            if (slot < 0 || slot > 5) continue;
            Slot& s = scene.slots[slot];
            s.image = f[0].empty() ? "" : chardir + f[0];
            s.mirrored = slot > 1;
            if (f.size() > 2 && to_int(f[2]) > 0) s.x = float(f.size() > 3 ? to_int(f[3]) : 0);
        } else if (starts(line, "move ", &rest)) {
            st.has_commands = true;
            const auto f = split_commas(rest);
            const int slot = to_int(f[0]);
            if (slot >= 0 && slot <= 5 && f.size() > 1) scene.slots[slot].x = float(to_int(f[1]));
        } else if (starts(line, "front ", &rest)) {
            st.has_commands = true;
            const int slot = to_int(rest);
            if (slot < 0 || slot > 5) continue;
            int* o = scene.order;
            std::stable_partition(o, o + 6, [&](int v) { return v != slot; });
        } else if (starts(line, "sleep ", &rest)) {
            pending_sleep += to_int(rest), st.has_commands = true;
        } else if (starts(line, "VOICE ", &rest)) {
            const std::string file = rest.substr(0, rest.find_first_of(", \t"));
            pending_voice = voicedir.empty() ? file : voicedir + file;
        } else {
            Page p;
            p.text = line;
            p.voice = pending_voice;
            p.scene = scene;
            p.sleep_ms = pending_sleep;
            st.pages.push_back(std::move(p));
            pending_voice.clear();
            pending_sleep = 0;
        }
    }
    return st;
}

}  // namespace story
}  // namespace db

namespace db {
namespace story {

std::vector<uint8_t> compose(const Scene& s,
                             bool (*load_rgba)(void*, const std::string&, std::vector<uint8_t>&, int&, int&),
                             void* ctx) {
    const int W = 800, H = 600;
    std::vector<uint8_t> out(size_t(W) * H * 4, 0);
    for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    // Straight-alpha blend of a whole image at (dx, dy), optionally mirrored.
    auto draw = [&](const std::string& name, int dx, int dy, bool mirror) {
        std::vector<uint8_t> img;
        int w = 0, h = 0;
        if (name.empty() || !load_rgba || !load_rgba(ctx, name, img, w, h) || img.size() != size_t(w) * size_t(h) * 4)
            return;
        for (int y = 0; y < h; y++) {
            const int oy = dy + y;
            if (oy < 0 || oy >= H) continue;
            for (int x = 0; x < w; x++) {
                const int ox = dx + x;
                if (ox < 0 || ox >= W) continue;
                const uint8_t* p = &img[size_t(y * w + (mirror ? w - 1 - x : x)) * 4];
                uint8_t* d = &out[size_t(oy * W + ox) * 4];
                const int a = p[3];
                for (int c = 0; c < 3; c++) d[c] = uint8_t((p[c] * a + d[c] * (255 - a) + 127) / 255);
            }
        }
    };
    draw(s.background, 0, 0, false);
    for (int k = 0; k < 6; k++) {
        const Slot& sl = s.slots[s.order[k]];
        draw(sl.image, int(sl.x) - 256, 88, sl.mirrored);
    }
    return out;
}

}  // namespace story
}  // namespace db
