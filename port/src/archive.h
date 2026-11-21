// Read-only access to Higurashi Daybreak daybreakNN.dat archives.
// Format: analysis/02-archive-format.md (reader 0x5E6D80, entry read 0x5E70A0).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace db {

struct Entry {
    std::string name;  // raw cp932 bytes, '/' separated
    uint32_t size = 0;
    uint32_t offset = 0;
};

// What the stage code reads from: one archive, or several searched in order.
class Source {
public:
    virtual ~Source() = default;
    // Case-insensitive (ASCII-only, like the game's _stricmp map) lookup.
    virtual const Entry* find(const std::string& name) const = 0;
    // `e` must come from this source's find().
    virtual bool read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const = 0;
};

class Archive : public Source {
public:
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    ~Archive() override;
    bool open(const std::string& path, std::string* err);
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry* find(const std::string& name) const override;
    bool read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const override;
    bool owns(const Entry& e) const;

private:
    FILE* f_ = nullptr;
    std::vector<Entry> entries_;
};

// Ordered search over attached archives, as File::Open does (DaybreakDX 0x6510a0):
// the first archive that contains the name wins. Kai attaches daybreak04 then
// daybreak00 (analysis/07-kai-archive-lookup.md); the base game uses daybreak00 alone.
// Archives are borrowed and must outlive the set.
class ArchiveSet : public Source {
public:
    void attach(const Archive* a) { archives_.push_back(a); }
    const std::vector<const Archive*>& archives() const { return archives_; }
    const Entry* find(const std::string& name) const override;
    // Index into archives() of the archive `name` resolves to, or -1.
    int which(const std::string& name) const;
    bool read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const override;

private:
    std::vector<const Archive*> archives_;
};

std::string lower_ascii(std::string s);

}  // namespace db
