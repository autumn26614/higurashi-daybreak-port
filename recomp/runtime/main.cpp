// Headless host for the recompiled daybreak.exe (with d3dx9_25.dll).
//   daybreak-recomp <folder with daybreak.exe>
#include <cstdlib>
#include <string>

#include <chrono>
#include <sstream>
#include <thread>

#include "audio.h"
#include "d3d9.h"
#include "game.h"
#include "host.h"
#include "input.h"
#include "runtime.h"
#include "soft.h"

namespace {
// Wraps the renderer: applies scripted key presses as frames are presented.
//   RECOMP_KEYS="120:1c:down,126:1c:up"   (frame:DIK hex:down|up)
class Scripted : public rt::d3d::Renderer {
public:
    Scripted(std::unique_ptr<rt::d3d::Renderer> inner, const char *keys) : inner_(std::move(inner)) {
        std::stringstream ss(keys ? keys : "");
        std::string item;
        while (std::getline(ss, item, ',')) {
            unsigned long long f;
            unsigned k;
            char what[8] = {0};
            double secs;
            if (item[0] == 't' && sscanf(item.c_str() + 1, "%lf:%x:%7s", &secs, &k, what) == 3) {
                // "t<seconds>:key:down|up": by wall-clock time (frames stop during movies)
                bool down = what[0] == 'd';
                std::thread([secs, k, down] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(long(secs * 1000)));
                    rt::input::set_key(k, down);
                }).detach();
            } else if (sscanf(item.c_str(), "%llu:%x:%7s", &f, &k, what) == 3) {
                events_.push_back({f, k, what[0] == 'd'});
            }
        }
    }
    void clear(rt::d3d::Device &d, uint32_t n, uint32_t r, uint32_t f, uint32_t c, float z, uint32_t s) override {
        inner_->clear(d, n, r, f, c, z, s);
    }
    void draw(rt::d3d::Device &d, const rt::d3d::Draw &dr) override { inner_->draw(d, dr); }
    void present(rt::d3d::Device &d) override {
        inner_->present(d);
        for (auto &e : events_) {
            if (e.frame != d.frames) continue;
            if (e.key >= 0x1000) {  // pad button (bit e.key - 0x1000)
                rt::input::Joystick j = rt::input::joystick();
                uint32_t bit = 1u << ((e.key - 0x1000) & 31);
                j.buttons = e.down ? (j.buttons | bit) : (j.buttons & ~bit);
                rt::input::set_joystick(j);
            } else {
                rt::input::set_key(e.key, e.down);
            }
        }
    }

private:
    struct Event {
        unsigned long long frame;
        unsigned key;
        bool down;
    };
    std::unique_ptr<rt::d3d::Renderer> inner_;
    std::vector<Event> events_;
};
}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder with daybreak.exe>\n", argv[0]);
        return 2;
    }
    rt::game_load(argv[1]);
    if (getenv("RECOMP_PAD")) {  // a connected game controller (scripted keys 1000+ press its buttons)
        rt::input::Joystick j;
        j.connected = true;
        rt::input::set_joystick(j);
    }
    rt::audio::start_headless();
    // RECOMP_SHOTS="<dir>:<every>[:<first>]" writes frames as PNG with the software renderer.
    std::unique_ptr<rt::d3d::Renderer> r;
    if (const char *shots = getenv("RECOMP_SHOTS")) {
        rt::d3d::SoftOptions o;
        std::string s = shots;
        size_t a = s.find(':');
        o.dir = s.substr(0, a);
        if (a != std::string::npos) {
            unsigned long long every = 0, first = 0;
            sscanf(s.c_str() + a + 1, "%llu:%llu", &every, &first);
            o.every = every;
            o.first = first;
        }
        r = rt::d3d::make_soft_renderer(o);
    }
    if (r || getenv("RECOMP_KEYS")) {
        if (!r) r = rt::d3d::make_soft_renderer({});
        rt::d3d::set_renderer(std::make_unique<Scripted>(std::move(r), getenv("RECOMP_KEYS")));
    }
    return rt::game_run();
}
