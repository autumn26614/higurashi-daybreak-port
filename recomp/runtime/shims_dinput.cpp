// DirectInput 8: the system keyboard (and mouse), fed by the host through rt::input.
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
}  // namespace input
}  // namespace rt

using namespace rt;

namespace {
constexpr uint32_t DI_OK = 0, DIERR_NOTFOUND = 0x80070002;
const char *kKeyboard = "{6F1D2B61-D5A0-11CF-BFC7-444553540000}";
const char *kMouse = "{6F1D2B60-D5A0-11CF-BFC7-444553540000}";

struct DIDevice : ComObject {
    bool keyboard = true;
};

uint32_t get_state(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t n = arg(c, 1), out = arg(c, 2);
    memset(GM + out, 0, n);
    if (d->keyboard) {
        std::lock_guard<std::mutex> lock(input::m);
        memcpy(GM + out, input::keys, std::min<uint32_t>(n, 256));
    }
    return DI_OK;
}

uint32_t get_data(CPU *c) {
    auto d = com_this<DIDevice>(c);
    uint32_t size = arg(c, 1), out = arg(c, 2), inout = arg(c, 3), flags = arg(c, 4);
    uint32_t want = ld32(inout), n = 0;
    if (d->keyboard) {
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

const Method device_vt[] = {
    COM_IUNKNOWN,
    {"GetCapabilities", 2, [](CPU *c) {
         uint32_t p = arg(c, 1);
         uint32_t size = ld32(p);
         memset(GM + p + 4, 0, size - 4);
         st32(p + 4, 1);   // attached
         st32(p + 8, com_this<DIDevice>(c)->keyboard ? 0x13 : 0x12);
         st32(p + 16, com_this<DIDevice>(c)->keyboard ? 128 : 3);  // buttons
         return DI_OK;
     }},
    {"EnumObjects", 4, [](CPU *) { return DI_OK; }},
    {"GetProperty", 3, [](CPU *) { return DIERR_NOTFOUND; }},
    {"SetProperty", 3, [](CPU *) { return DI_OK; }},
    {"Acquire", 1, [](CPU *) { return DI_OK; }},
    {"Unacquire", 1, [](CPU *) { return DI_OK; }},
    {"GetDeviceState", 3, get_state},
    {"GetDeviceData", 5, get_data},
    {"SetDataFormat", 2, [](CPU *) { return DI_OK; }},
    {"SetEventNotification", 2, [](CPU *) { return DI_OK; }},
    {"SetCooperativeLevel", 3, [](CPU *) { return DI_OK; }},
    {"GetObjectInfo", 4, nullptr}, {"GetDeviceInfo", 2, nullptr}, {"RunControlPanel", 3, nullptr},
    {"Initialize", 4, nullptr}, {"CreateEffect", 5, nullptr},
    {"EnumEffects", 4, [](CPU *) { return DI_OK; }},
    {"GetEffectInfo", 3, nullptr}, {"GetForceFeedbackState", 2, nullptr}, {"SendForceFeedbackCommand", 2, nullptr},
    {"EnumCreatedEffectObjects", 4, nullptr}, {"Escape", 2, nullptr},
    {"Poll", 1, [](CPU *) { return 1u; }},  // DI_NOEFFECT: nothing to poll
    {"SendDeviceData", 5, nullptr}, {"EnumEffectsInFile", 5, nullptr}, {"WriteEffectToFile", 5, nullptr},
    {"BuildActionMap", 4, nullptr}, {"SetActionMap", 4, nullptr}, {"GetImageInfo", 2, nullptr},
};

struct DInput : ComObject {};
const Method dinput_vt[] = {
    COM_IUNKNOWN,
    {"CreateDevice", 4, [](CPU *c) {
         std::string g = guid_string(arg(c, 1));
         if (g != kKeyboard && g != kMouse) {
             log("recomp: DirectInput CreateDevice %s: not available", g.c_str());
             st32(arg(c, 2), 0);
             return DIERR_NOTFOUND;
         }
         auto d = std::make_shared<DIDevice>();
         d->keyboard = g == kKeyboard;
         d->iids = {"{54D41080-DC15-4833-A41B-748F73A38179}"};
         st32(arg(c, 2), com_new(d, com_vtable("IDirectInputDevice8A", device_vt)));
         return DI_OK;
     }},
    {"EnumDevices", 5, [](CPU *) { return DI_OK; }},  // no game controllers (yet)
    {"GetDeviceStatus", 2, [](CPU *c) { return guid_string(arg(c, 1)) == kKeyboard ? DI_OK : 1u; }},
    {"RunControlPanel", 3, nullptr},
    {"Initialize", 3, [](CPU *) { return DI_OK; }},
    {"FindDevice", 4, nullptr}, {"EnumDevicesBySemantics", 6, nullptr}, {"ConfigureDevices", 5, nullptr},
};
}  // namespace

SHIM(dinput8, DirectInput8Create, 20) {
    auto o = std::make_shared<DInput>();
    o->iids = {"{BF798030-483A-4DA2-AA99-5D64ED369700}"};
    st32(arg(c, 3), com_new(o, com_vtable("IDirectInput8A", dinput_vt)));
    return DI_OK;
}
