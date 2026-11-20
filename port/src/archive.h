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

class Archive {
public:
    ~Archive();
    bool open(const std::string& path, std::string* err);
    const std::vector<Entry>& entries() const { return entries_; }
    // Case-insensitive (ASCII-only, like the game's _stricmp map) lookup.
    const Entry* find(const std::string& name) const;
    bool read(const Entry& e, std::vector<uint8_t>& out, std::string* err) const;

private:
    FILE* f_ = nullptr;
    std::vector<Entry> entries_;
};

std::string lower_ascii(std::string s);

}  // namespace db
