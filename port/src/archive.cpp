#include "archive.h"

#include <cstring>
#include <functional>

namespace db {

static const size_t kEntrySize = 0x10C;

std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

Archive::~Archive() { close(); }

namespace {

void set_err(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

int seek64(FILE* f, uint64_t pos, int whence) {
#ifdef _WIN32
    return _fseeki64(f, int64_t(pos), whence);
#else
    return fseeko(f, off_t(pos), whence);
#endif
}

int64_t tell64(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return int64_t(ftello(f));
#endif
}

}  // namespace

void Archive::close() {
    if (f_) fclose(f_);
    f_ = nullptr;
    entries_.clear();
    lookup_.clear();
}

bool Archive::open(const std::string& path, std::string* err) {
    close();
    f_ = fopen(path.c_str(), "rb");
    if (!f_) {
        set_err(err, "cannot open " + path);
        return false;
    }
    if (!load_index(path, err)) {
        close();
        return false;
    }
    return true;
}

bool Archive::load_index(const std::string& path, std::string* err) {
    if (seek64(f_, 0, SEEK_END) != 0) {
        set_err(err, "cannot seek " + path);
        return false;
    }
    const int64_t file_size = tell64(f_);
    if (file_size < 0 || seek64(f_, 0, SEEK_SET) != 0) {
        set_err(err, "cannot seek " + path);
        return false;
    }
    file_size_ = uint64_t(file_size);
    uint8_t cnt[2];
    if (fread(cnt, 1, 2, f_) != 2) {
        set_err(err, "short read (count)");
        return false;
    }
    size_t count = size_t(cnt[0]) | (size_t(cnt[1]) << 8);
    const uint64_t index_end = 2 + uint64_t(count) * kEntrySize;
    if (index_end > file_size_) {
        set_err(err, "short read (index)");
        return false;
    }
    std::vector<uint8_t> idx(count * kEntrySize);
    if (fread(idx.data(), 1, idx.size(), f_) != idx.size()) {
        set_err(err, "short read (index)");
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
    lookup_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = &idx[i * kEntrySize];
        size_t n = strnlen(reinterpret_cast<const char*>(e), 0x104);
        if (n == 0 || n == 0x104) {
            set_err(err, "bad index: entry " + std::to_string(i) + (n ? " name is not terminated" : " has no name"));
            return false;
        }
        Entry& out = entries_[i];
        out.name.assign(reinterpret_cast<const char*>(e), n);
        out.size = load_le32(e + 0x104);
        out.offset = load_le32(e + 0x108);
        // The payload must lie after the index and inside the file. Checked here so that
        // read() never allocates for a size the file cannot hold.
        if (out.offset < index_end || uint64_t(out.offset) + out.size > file_size_) {
            set_err(err, "bad index: entry " + std::to_string(i) + " (" + out.name + ") lies outside the file");
            return false;
        }
        // Duplicate names: the first one wins, as in the game's map insert.
        lookup_.emplace(lower_ascii(out.name), i);
    }
    return true;
}

const Entry* Archive::find(const std::string& name) const {
    auto it = lookup_.find(lower_ascii(name));
    return it == lookup_.end() ? nullptr : &entries_[it->second];
}

bool Archive::owns(const Entry& e) const {
    // std::less gives a total order even for pointers into other arrays.
    std::less<const Entry*> lt;
    return !entries_.empty() && !lt(&e, entries_.data()) && lt(&e, entries_.data() + entries_.size());
}

bool Archive::read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const {
    if (!f_) {
        set_err(err, "archive not open");
        return false;
    }
    if (uint64_t(e.offset) + e.size > file_size_) {
        set_err(err, "entry outside the file: " + e.name);
        return false;
    }
    out.resize(e.size);
    {
        std::lock_guard<std::mutex> lock(read_);  // music decodes off the main thread
        if (seek64(f_, e.offset, SEEK_SET) != 0 || fread(out.data(), 1, e.size, f_) != e.size) {
            set_err(err, "short read: " + e.name);
            return false;
        }
    }
    // Payload XOR used by Archive::Read (0x5E70A0): one byte key per entry.
    uint8_t k = uint8_t((e.offset >> 1) | 8);
    for (auto& b : out) b ^= k;
    return true;
}

bool Source::read_range(const Entry& e, uint64_t at, size_t count, std::vector<uint8_t>& out,
                        std::string* err) const {
    std::vector<uint8_t> all;
    if (!read(e, all, err)) return false;
    const uint64_t from = std::min<uint64_t>(at, all.size());
    const uint64_t to = std::min<uint64_t>(from + count, all.size());
    out.assign(all.begin() + std::ptrdiff_t(from), all.begin() + std::ptrdiff_t(to));
    return true;
}

bool Archive::read_range(const Entry& e, uint64_t at, size_t count, std::vector<uint8_t>& out,
                         std::string* err) const {
    if (!f_) {
        set_err(err, "archive not open");
        return false;
    }
    if (uint64_t(e.offset) + e.size > file_size_) {
        set_err(err, "entry outside the file: " + e.name);
        return false;
    }
    const uint64_t from = std::min<uint64_t>(at, e.size);
    const size_t n = size_t(std::min<uint64_t>(count, e.size - from));
    out.resize(n);
    if (n > 0) {
        std::lock_guard<std::mutex> lock(read_);
        if (seek64(f_, int64_t(e.offset + from), SEEK_SET) != 0 || fread(out.data(), 1, n, f_) != n) {
            set_err(err, "short read: " + e.name);
            return false;
        }
    }
    // The payload XOR (0x5E70A0) is one byte for the whole entry, so any range decrypts alone.
    const uint8_t k = uint8_t((e.offset >> 1) | 8);
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

bool ArchiveSet::read_range(const Entry& e, uint64_t at, size_t count, std::vector<uint8_t>& out,
                            std::string* err) const {
    for (const Archive* a : archives_)
        if (a->owns(e)) return a->read_range(e, at, count, out, err);
    if (err) *err = "entry not from this archive set: " + e.name;
    return false;
}

bool ArchiveSet::read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const {
    // Each archive has its own file and offsets, so read through the one that owns e.
    for (const Archive* a : archives_)
        if (a->owns(e)) return a->read(e, out, err);
    if (err) *err = "entry not from this archive set: " + e.name;
    return false;
}

}  // namespace db
