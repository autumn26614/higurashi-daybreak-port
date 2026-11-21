#include "archive.h"

#include <cstring>

namespace db {

static const size_t kEntrySize = 0x10C;

std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

Archive::~Archive() {
    if (f_) fclose(f_);
}

bool Archive::open(const std::string& path, std::string* err) {
    f_ = fopen(path.c_str(), "rb");
    if (!f_) {
        *err = "cannot open " + path;
        return false;
    }
    uint8_t cnt[2];
    if (fread(cnt, 1, 2, f_) != 2) {
        *err = "short read (count)";
        return false;
    }
    size_t count = size_t(cnt[0]) | (size_t(cnt[1]) << 8);
    std::vector<uint8_t> idx(count * kEntrySize);
    if (fread(idx.data(), 1, idx.size(), f_) != idx.size()) {
        *err = "short read (index)";
        return false;
    }
    // Rolling XOR used by Archive::Open (0x5E6D80): key=100, step=100, step+=0x4D.
    uint8_t key = 100, step = 100;
    for (auto& b : idx) {
        b ^= key;
        key = uint8_t(key + step);
        step = uint8_t(step + 0x4D);
    }
    entries_.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = &idx[i * kEntrySize];
        size_t n = strnlen(reinterpret_cast<const char*>(e), 0x104);
        entries_[i].name.assign(reinterpret_cast<const char*>(e), n);
        memcpy(&entries_[i].size, e + 0x104, 4);    // little-endian host assumed (x86-64 / arm64)
        memcpy(&entries_[i].offset, e + 0x108, 4);
    }
    return true;
}

const Entry* Archive::find(const std::string& name) const {
    std::string want = lower_ascii(name);
    for (const auto& e : entries_)
        if (lower_ascii(e.name) == want) return &e;
    return nullptr;
}

bool Archive::owns(const Entry& e) const {
    return !entries_.empty() && &e >= entries_.data() && &e < entries_.data() + entries_.size();
}

bool Archive::read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const {
    if (!f_) {
        *err = "archive not open";
        return false;
    }
    out.resize(e.size);
#ifdef _WIN32
    int rc = _fseeki64(f_, int64_t(e.offset), SEEK_SET);
#else
    int rc = fseeko(f_, off_t(e.offset), SEEK_SET);
#endif
    if (rc != 0 || fread(out.data(), 1, e.size, f_) != e.size) {
        *err = "short read: " + e.name;
        return false;
    }
    // Payload XOR used by Archive::Read (0x5E70A0): one byte key per entry.
    uint8_t k = uint8_t((e.offset >> 1) | 8);
    for (auto& b : out) b ^= k;
    return true;
}

const Entry* ArchiveSet::find(const std::string& name) const {
    for (const Archive* a : archives_)
        if (const Entry* e = a->find(name)) return e;
    return nullptr;
}

int ArchiveSet::which(const std::string& name) const {
    for (size_t i = 0; i < archives_.size(); ++i)
        if (archives_[i]->find(name)) return int(i);
    return -1;
}

bool ArchiveSet::read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const {
    // Each archive has its own file and offsets, so read through the one that owns e.
    for (const Archive* a : archives_)
        if (a->owns(e)) return a->read(e, out, err);
    *err = "entry not from this archive set: " + e.name;
    return false;
}

}  // namespace db
