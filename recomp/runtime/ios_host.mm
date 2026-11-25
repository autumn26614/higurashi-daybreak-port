// The iOS app's entry into the recompiled game (recomp_host.h).
#if defined(__APPLE__)
#include "recomp_host.h"

#include <pthread.h>

#include <string>

#include "audio.h"
#include "d3d9.h"
#include "game.h"
#include "input.h"
#include "metal.h"
#include "runtime.h"

namespace {
struct StartArgs {
    std::string dir;
};

void *game_thread(void *p) {
    StartArgs *a = static_cast<StartArgs *>(p);
    rt::game_load(a->dir);
    int code = rt::game_run();
    rt::log("recomp: the game ended (%d)", code);
    delete a;
    return nullptr;
}
}  // namespace

extern "C" {

int recomp_has_game(void) { return rt::game_translated() ? 1 : 0; }

int recomp_files_present(const char *dir) { return rt::game_files_present(dir) ? 1 : 0; }

void recomp_start(const char *dir, void *layer) {
    static bool started = false;
    if (started || !rt::game_translated()) return;
    started = true;
    rt::d3d::set_renderer(rt::d3d::make_metal_renderer(layer));
    // Translated code recurses on the host stack as deeply as the game does on its own: a
    // big stack, smaller if the system will not give it.
    auto *args = new StartArgs{dir};
    for (size_t mib : {64, 32, 16}) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, mib << 20);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        int err = pthread_create(&t, &attr, game_thread, args);
        pthread_attr_destroy(&attr);
        if (err == 0) return;
        rt::log("recomp: no game thread with a %zu MiB stack (%d)", mib, err);
    }
    rt::fatal("cannot start the game thread");
}

void recomp_key(uint32_t dik, int down) { rt::input::set_key(dik, down != 0); }

void recomp_audio(int16_t *out, int frames) { rt::audio::mix(out, frames); }

void recomp_joystick(int connected, float x, float y, float rx, float ry, uint32_t buttons, int pov) {
    rt::input::Joystick j;
    j.connected = connected != 0;
    j.x = x;
    j.y = y;
    j.rx = rx;
    j.ry = ry;
    j.buttons = buttons;
    j.pov = pov;
    rt::input::set_joystick(j);
}

}  // extern "C"
#endif
