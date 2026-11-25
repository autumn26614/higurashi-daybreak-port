// DirectInput 8: the system keyboard and one game controller, fed by the host through
// rt::input (touch controls and keys; a connected pad).
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "host.h"
#include "input.h"
#include "win.h"

namespace rt {
namespace input {
namespace {
std::mutex m;
uint8_t keys[256];
struct Event {
    uint32_t key, data, seq;
};
std::deque<Event> events;
uint32_t seq = 0;
Joystick joy;
}  // namespace

void set_key(uint32_t dik, bool down) {
    std::lock_guard<std::mutex> lock(m);
    uint8_t v = down ? 0x80 : 0;
    if (dik >= 256 || keys[dik] == v) return;
    keys[dik] = v;
    events.push_back({dik, v, seq++});
    if (events.size() > 256) events.pop_front();
}

bool key(uint32_t dik) {
    std::lock_guard<std::mutex> lock(m);
    return dik < 256 && keys[dik];
}

void set_joystick(const Joystick &j) {
    std::lock_guard<std::mutex> lock(m);
    joy = j;
}

Joystick joystick() {
    std::lock_guard<std::mutex> lock(m);
    return joy;
}
}  // namespace input
}  // namespace rt

using namespace rt;

namespace {
constexpr uint32_t DI_OK = 0, DIERR_NOTFOUND = 0x80070002, DIERR_INVALIDPARAM = 0x80070057;
const char *kKeyboard = "{6F1D2B61-D5A0-11CF-BFC7-444553540000}";
const char *kMouse = "{6F1D2B60-D5A0-11CF-BFC7-444553540000}";
// The pad we report: instance and product GUIDs of our own.
const uint8_t kPadGuid[16] = {0x52, 0x43, 0x4D, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44};
const char *kPad = "{504D4352-0000-0000-0000-504944564944}";

enum class Kind { keyboard, mouse, pad };

struct DIDevice : ComObject {
    Kind kind = Kind::keyboard;
    int32_t lo = 0, hi = 65535;  // pad axis range (DIPROP_RANGE)
};

int32_t axis(float v, const DIDevice &d) {
    v = std::fmax(-1.0f, std::fmin(1.0f, v));
    double mid = (double(d.lo) + double(d.hi)) / 2, half = (double(d.hi) - double(d.lo)) / 2;
    return int32_t(std::lround(mid + v * half));
}

uint32_t get_state(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t n = arg(c, 1), out = arg(c, 2);
    memset(GM + out, 0, n);
    if (d->kind == Kind::keyboard) {
        std::lock_guard<std::mutex> lock(input::m);
        memcpy(GM + out, input::keys, std::min<uint32_t>(n, 256));
    } else if (d->kind == Kind::pad && n >= 80) {
        input::Joystick j = input::joystick();
        // DIJOYSTATE: lX lY lZ lRx lRy lRz, 2 sliders, 4 POVs, 32 buttons.
        st32(out + 0, uint32_t(axis(j.x, *d)));
        st32(out + 4, uint32_t(axis(j.y, *d)));
        st32(out + 8, uint32_t(axis(j.z, *d)));
        st32(out + 12, uint32_t(axis(j.rx, *d)));
        st32(out + 16, uint32_t(axis(j.ry, *d)));
        st32(out + 20, uint32_t(axis(j.rz, *d)));
        st32(out + 32, j.pov < 0 ? 0xFFFFFFFF : uint32_t(j.pov) * 100);
        for (int i = 1; i < 4; i++) st32(out + 32 + 4 * uint32_t(i), 0xFFFFFFFF);
        for (int i = 0; i < 32; i++) st8(out + 48 + uint32_t(i), (j.buttons >> i) & 1 ? 0x80 : 0);
    }
    return DI_OK;
}

uint32_t get_data(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t size = arg(c, 1), out = arg(c, 2), inout = arg(c, 3), flags = arg(c, 4);
    uint32_t want = ld32(inout), n = 0;
    if (d->kind == Kind::keyboard) {
        std::lock_guard<std::mutex> lock(input::m);
        while (n < want && !input::events.empty()) {
            const auto &e = input::events.front();
            if (out) {
                uint32_t o = out + n * size;
                memset(GM + o, 0, size);
                st32(o, e.key);
                st32(o + 4, e.data);
                st32(o + 8, uint32_t(now_us() / 1000));
                st32(o + 12, e.seq);
            }
            if (!(flags & 1)) input::events.pop_front();  // DIGDD_PEEK leaves them
            n++;
            if (flags & 1) break;
        }
        if (!out && want == 0xFFFFFFFF) {  // flush
            n = uint32_t(input::events.size());
            input::events.clear();
        }
    }
    st32(inout, n);
    return DI_OK;
}

uint32_t set_property(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t prop = arg(c, 1), hdr = arg(c, 2);
    if (prop == 4 && hdr) {  // DIPROP_RANGE: DIPROPHEADER (16 bytes), lMin, lMax
        d->lo = int32_t(ld32(hdr + 16));
        d->hi = int32_t(ld32(hdr + 20));
    }
    return DI_OK;
}

uint32_t get_property(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t prop = arg(c, 1), hdr = arg(c, 2);
    if (prop == 4 && hdr && d->kind == Kind::pad) {
        st32(hdr + 16, uint32_t(d->lo));
        st32(hdr + 20, uint32_t(d->hi));
        return DI_OK;
    }
    return DIERR_NOTFOUND;
}

void write_instance(uint32_t p) {
    // DIDEVICEINSTANCEA: dwSize, guidInstance, guidProduct, dwDevType, names[260] x2, guidFFDriver, usage page/usage.
    uint32_t size = ld32(p) ? ld32(p) : 0x244;
    memset(GM + p + 4, 0, size - 4);
    st32(p, size);
    memcpy(GM + p + 4, kPadGuid, 16);
    memcpy(GM + p + 20, kPadGuid, 16);
    st32(p + 36, 0x0115);  // DI8DEVTYPE_GAMEPAD, standard subtype
    strcpy(gptr(p + 40), "Game Controller");
    strcpy(gptr(p + 300), "Game Controller");
    st16(p + 576, 1);   // HID usage page: generic desktop
    st16(p + 578, 5);   // usage: game pad
}

const Method device_vt[] = {
    COM_IUNKNOWN,
    {"GetCapabilities", 2, [](CPU *c) {
         auto d = com_this<DIDevice>(c);
         uint32_t p = arg(c, 1);
         uint32_t size = ld32(p);
         memset(GM + p + 4, 0, size - 4);
         st32(p + 4, 1);  // DIDC_ATTACHED
         if (d->kind == Kind::pad) {
             st32(p + 8, 0x0115);
             st32(p + 12, 6);   // axes
             st32(p + 16, 16);  // buttons
             st32(p + 20, 1);   // POVs
         } else {
             st32(p + 8, d->kind == Kind::keyboard ? 0x13 : 0x12);
             st32(p + 16, d->kind == Kind::keyboard ? 128 : 3);
         }
         return DI_OK;
     }},
    {"EnumObjects", 4, [](CPU *) { return DI_OK; }},
    {"GetProperty", 3, get_property},
    {"SetProperty", 3, set_property},
    {"Acquire", 1, [](CPU *) { return DI_OK; }},
    {"Unacquire", 1, [](CPU *) { return DI_OK; }},
    {"GetDeviceState", 3, get_state},
    {"GetDeviceData", 5, get_data},
    {"SetDataFormat", 2, [](CPU *) { return DI_OK; }},
    {"SetEventNotification", 2, [](CPU *) { return DI_OK; }},
    {"SetCooperativeLevel", 3, [](CPU *) { return DI_OK; }},
    {"GetObjectInfo", 4, nullptr},
    {"GetDeviceInfo", 2, [](CPU *c) {
         write_instance(arg(c, 1));
         return DI_OK;
     }},
    {"RunControlPanel", 3, nullptr}, {"Initialize", 4, nullptr}, {"CreateEffect", 5, nullptr},
    {"EnumEffects", 4, [](CPU *) { return DI_OK; }},
    {"GetEffectInfo", 3, nullptr}, {"GetForceFeedbackState", 2, nullptr}, {"SendForceFeedbackCommand", 2, nullptr},
    {"EnumCreatedEffectObjects", 4, nullptr}, {"Escape", 2, nullptr},
    {"Poll", 1, [](CPU *) { return 1u; }},  // DI_NOEFFECT
    {"SendDeviceData", 5, nullptr}, {"EnumEffectsInFile", 5, nullptr}, {"WriteEffectToFile", 5, nullptr},
    {"BuildActionMap", 4, nullptr}, {"SetActionMap", 4, nullptr}, {"GetImageInfo", 2, nullptr},
};

struct DInput : ComObject {};
const Method dinput_vt[] = {
    COM_IUNKNOWN,
    {"CreateDevice", 4, [](CPU *c) {
         std::string g = guid_string(arg(c, 1));
         auto d = std::make_shared<DIDevice>();
         if (g == kKeyboard) d->kind = Kind::keyboard;
         else if (g == kMouse) d->kind = Kind::mouse;
         else if (g == kPad) d->kind = Kind::pad;
         else {
             log("recomp: DirectInput CreateDevice %s: not available", g.c_str());
             st32(arg(c, 2), 0);
             return DIERR_NOTFOUND;
         }
         d->iids = {"{54D41080-DC15-4833-A41B-748F73A38179}"};
         st32(arg(c, 2), com_new(d, com_vtable("IDirectInputDevice8A", device_vt)));
         return DI_OK;
     }},
    {"EnumDevices", 5, [](CPU *c) {
         // Game controllers (DI8DEVCLASS_GAMECTRL 4, or all 0): our pad, when one is connected.
         uint32_t type = arg(c, 1), cb = arg(c, 2), ref = arg(c, 3);
         if ((type == 0 || type == 4 || type == 0x15) && cb && input::joystick().connected) {
             uint32_t inst = galloc(0x244, true);
             st32(inst, 0x244);
             write_instance(inst);
             call_guest(c, cb, {inst, ref});
             gfree(inst);
         }
         return DI_OK;
     }},
    {"GetDeviceStatus", 2, [](CPU *c) {
         std::string g = guid_string(arg(c, 1));
         if (g == kKeyboard) return DI_OK;
         if (g == kPad) return input::joystick().connected ? DI_OK : 1u;
         return 1u;  // DI_NOTATTACHED
     }},
    {"RunControlPanel", 3, nullptr},
    {"Initialize", 3, [](CPU *) { return DI_OK; }},
    {"FindDevice", 4, [](CPU *) { return DIERR_INVALIDPARAM; }},
    {"EnumDevicesBySemantics", 6, nullptr}, {"ConfigureDevices", 5, nullptr},
};
}  // namespace

SHIM(dinput8, DirectInput8Create, 20) {
    auto o = std::make_shared<DInput>();
    o->iids = {"{BF798030-483A-4DA2-AA99-5D64ED369700}"};
    st32(arg(c, 3), com_new(o, com_vtable("IDirectInput8A", dinput_vt)));
    return DI_OK;
}
