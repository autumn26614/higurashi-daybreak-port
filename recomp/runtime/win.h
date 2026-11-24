// The window, its message queue, and COM objects backed by host code.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "runtime.h"

namespace rt {

struct Msg {
    uint32_t hwnd, message, wparam, lparam;
};
struct WindowState {
    uint32_t hwnd = 0, wndproc = 0;
    uint32_t width = 800, height = 600;
    std::function<void(CPU *)> on_idle;  // called while the game polls for messages
};
extern WindowState window;
void post_message(const Msg &m);
uint32_t send_message(CPU *c, uint32_t msg, uint32_t wparam, uint32_t lparam);

// ---- COM ----
// A guest object is a guest block whose first word points to a guest vtable of thunks;
// the host object behind it is found by the guest address (`this`).
std::string guid_string(uint32_t guid);  // "{xxxxxxxx-xxxx-...}" upper case

struct ComObject {
    virtual ~ComObject() = default;
    uint32_t self = 0;  // guest address
    uint32_t refs = 1;
    std::vector<std::string> iids;  // interfaces QueryInterface hands out as `self`
};

struct Method {
    const char *name;
    int nargs;  // stack arguments including `this`
    ShimFn fn;  // null: not implemented (fatal when called)
};

// Guest vtable for an interface (built once per name).
uint32_t com_vtable(const std::string &iface, const Method *methods, size_t count);
template <size_t N> uint32_t com_vtable(const std::string &iface, const Method (&m)[N]) {
    return com_vtable(iface, m, N);
}
// Registers a host object under a new guest object with vtable `vt` (and `extra` bytes of
// guest storage after the vtable pointer); returns the guest address.
uint32_t com_new(std::shared_ptr<ComObject> o, uint32_t vt, uint32_t extra = 0);
std::shared_ptr<ComObject> com_get(uint32_t self);
template <class T> std::shared_ptr<T> com_as(uint32_t self) { return std::dynamic_pointer_cast<T>(com_get(self)); }
template <class T> std::shared_ptr<T> com_this(CPU *c) {
    auto p = com_as<T>(arg(c, 0));
    if (!p) fatal("COM call on unknown object %08x", arg(c, 0));
    return p;
}
void com_release_all(uint32_t self);

// Standard IUnknown methods for host objects.
uint32_t com_query_interface(CPU *c);
uint32_t com_add_ref(CPU *c);
uint32_t com_release(CPU *c);
#define COM_IUNKNOWN {"QueryInterface", 3, ::rt::com_query_interface}, {"AddRef", 1, ::rt::com_add_ref}, \
    {"Release", 1, ::rt::com_release}

// CoCreateInstance: 0 if the class is not provided.
uint32_t com_create(CPU *c, const std::string &clsid, const std::string &iid);
using ComFactory = std::function<uint32_t(CPU *, const std::string &iid)>;
struct ComClassReg {
    ComClassReg(const char *clsid, ComFactory f);
};

}  // namespace rt
