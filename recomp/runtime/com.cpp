#include <cstring>

#include "win.h"

namespace rt {

std::string guid_string(uint32_t g) {
    if (!g) return "{null}";
    char buf[64];
    snprintf(buf, sizeof buf, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", ld32(g), ld16(g + 4),
             ld16(g + 6), ld8(g + 8), ld8(g + 9), ld8(g + 10), ld8(g + 11), ld8(g + 12), ld8(g + 13), ld8(g + 14),
             ld8(g + 15));
    return buf;
}

namespace {
std::mutex com_mutex;
std::map<uint32_t, std::shared_ptr<ComObject>> objects;
std::map<std::string, uint32_t> vtables;
std::map<std::string, ComFactory> &classes() {
    static std::map<std::string, ComFactory> c;
    return c;
}
}  // namespace

uint32_t com_vtable(const std::string &iface, const Method *methods, size_t count) {
    {
        std::lock_guard<std::mutex> lock(com_mutex);
        auto it = vtables.find(iface);
        if (it != vtables.end()) return it->second;
    }
    uint32_t vt = galloc(uint32_t(4 * count));
    for (size_t i = 0; i < count; i++) {
        const Method &m = methods[i];
        std::string name = iface + "::" + m.name;
        st32(vt + 4 * uint32_t(i), make_thunk(name, 4 * m.nargs, m.fn));
    }
    std::lock_guard<std::mutex> lock(com_mutex);
    vtables[iface] = vt;
    return vt;
}

uint32_t com_new(std::shared_ptr<ComObject> o, uint32_t vt, uint32_t extra) {
    uint32_t self = galloc(4 + extra, true);
    st32(self, vt);
    o->self = self;
    std::lock_guard<std::mutex> lock(com_mutex);
    objects[self] = std::move(o);
    return self;
}

std::shared_ptr<ComObject> com_get(uint32_t self) {
    std::lock_guard<std::mutex> lock(com_mutex);
    auto it = objects.find(self);
    return it == objects.end() ? nullptr : it->second;
}

void com_release_all(uint32_t self) {
    std::shared_ptr<ComObject> o;
    {
        std::lock_guard<std::mutex> lock(com_mutex);
        auto it = objects.find(self);
        if (it == objects.end()) return;
        o = it->second;
        objects.erase(it);
    }
    gfree(self);
}

uint32_t com_query_interface(CPU *c) {
    auto o = com_get(arg(c, 0));
    std::string iid = guid_string(arg(c, 1));
    if (o) {
        bool ok = iid == "{00000000-0000-0000-C000-000000000046}";  // IUnknown
        for (auto &i : o->iids) ok = ok || i == iid;
        if (ok) {
            o->refs++;
            st32(arg(c, 2), o->self);
            return 0;
        }
    }
    log("recomp: QueryInterface(%08x, %s): no such interface", arg(c, 0), iid.c_str());
    st32(arg(c, 2), 0);
    return 0x80004002;  // E_NOINTERFACE
}

uint32_t com_add_ref(CPU *c) {
    auto o = com_get(arg(c, 0));
    return o ? ++o->refs : 0;
}

uint32_t com_release(CPU *c) {
    auto o = com_get(arg(c, 0));
    if (!o) return 0;
    uint32_t r = --o->refs;
    if (r == 0) com_release_all(o->self);
    return r;
}

ComClassReg::ComClassReg(const char *clsid, ComFactory f) { classes()[clsid] = std::move(f); }

uint32_t com_create(CPU *c, const std::string &clsid, const std::string &iid) {
    auto it = classes().find(clsid);
    return it == classes().end() ? 0 : it->second(c, iid);
}

}  // namespace rt
