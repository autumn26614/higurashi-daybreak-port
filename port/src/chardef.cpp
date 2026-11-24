#include "chardef.h"

#include <cstring>

namespace db {

namespace {

struct Reader {
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;

    int32_t i32() {
        if (at + 4 > n) {
            ok = false;
            return 0;
        }
        int32_t v;
        std::memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    std::string str(size_t len) {
        if (at + len > n) {
            ok = false;
            return {};
        }
        const char* s = reinterpret_cast<const char*>(p + at);
        at += len;
        return std::string(s, strnlen(s, len));
    }
    void raw(uint8_t* out, size_t len) {
        if (at + len > n) {
            ok = false;
            return;
        }
        std::memcpy(out, p + at, len);
        at += len;
    }
    // Counts are bounded by the bytes left so a damaged file cannot ask for a huge allocation.
    uint32_t count(size_t min_item) {
        const int32_t c = i32();
        if (!ok || c < 0 || size_t(c) * min_item > n - at) {
            ok = false;
            return 0;
        }
        return uint32_t(c);
    }
};

}  // namespace

int32_t FrameRecord::param(int offset) const {
    if (offset < 0 || size_t(offset) + 4 > params.size()) return 0;
    int32_t v;
    std::memcpy(&v, params.data() + offset, 4);
    return v;
}

const Move* CharDef::move(int32_t id) const {
    for (const Move& m : moves)
        if (m.id == id) return &m;
    return nullptr;
}

namespace {

bool parse_def(const uint8_t* data, size_t size, bool head, CharDef& out, std::string* err) {
    out = CharDef{};
    Reader r{data, size};
    r.i32();
    out.model = r.str(0x104);
    if (head)
        for (auto& h : out.head) h = r.i32();
    const uint32_t nmoves = r.count(16);
    for (uint32_t i = 0; r.ok && i < nmoves; ++i) {
        Move m;
        m.id = r.i32();
        m.rank = r.i32();
        m.cancel = r.i32();
        const uint32_t nsteps = r.count(8);
        for (uint32_t s = 0; r.ok && s < nsteps; ++s) {
            MoveStep st;
            st.anim = r.i32();
            const uint32_t nframes = r.count(154);
            for (uint32_t f = 0; r.ok && f < nframes; ++f) {
                FrameRecord fr;
                fr.bone = r.str(30);
                fr.from = r.i32();
                for (auto& v : fr.pos) v = r.i32();
                fr.radius = r.i32();
                r.raw(fr.params.data(), fr.params.size());
                fr.flags = r.i32();
                fr.f16 = r.i32();
                fr.hit = r.i32();
                for (auto& v : fr.size) v = r.i32();
                st.frames.push_back(std::move(fr));
            }
            m.steps.push_back(std::move(st));
        }
        out.moves.push_back(std::move(m));
    }
    const uint32_t nanims = r.count(12);
    for (uint32_t i = 0; r.ok && i < nanims; ++i) {
        AnimDef a;
        a.blend = r.i32();
        a.loop = r.i32();
        const uint32_t nev = r.count(132);
        for (uint32_t e = 0; r.ok && e < nev; ++e) {
            AnimEvent ev;
            ev.from = r.i32();
            ev.to = r.i32();
            ev.period = r.i32();
            ev.texture = r.str(60);
            ev.swap = r.str(60);
            a.events.push_back(std::move(ev));
        }
        out.anims.push_back(std::move(a));
    }
    if (!r.ok) {
        if (err) *err = "character definition is truncated";
        return false;
    }
    if (r.at != size) {
        if (err) *err = "character definition has trailing bytes";
        return false;
    }
    return true;
}

}  // namespace

bool parse_chardef(const uint8_t* data, size_t size, CharDef& out, std::string* err) {
    return parse_def(data, size, true, out, err);
}

bool parse_bulletdef(const uint8_t* data, size_t size, CharDef& out, std::string* err) {
    return parse_def(data, size, false, out, err);
}

}  // namespace db
