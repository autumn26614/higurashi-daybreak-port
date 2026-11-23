#include "identify.h"

#include <algorithm>

namespace db {

namespace {

struct Known {
    ArchiveRole role;
    size_t entries;
    const char* member;  // an entry this archive holds
    const char* file;
};

// 01 and 05 are both BGM archives, 02 and 06 both movies: the counts tell them apart.
const Known kKnown[] = {
    {ArchiveRole::Base00, 7677, "data/stage01/00.x", "daybreak00.dat"},
    {ArchiveRole::Base01, 37, "data/bgm/win.ogg", "daybreak01.dat"},
    {ArchiveRole::Base02, 10, "movie/staffroll.mpg", "daybreak02.dat"},
    {ArchiveRole::Kai04, 4434, "data/stage16/00.x", "daybreak04.dat"},
    {ArchiveRole::Kai05, 41, "data/bgm/win.ogg", "daybreak05.dat"},
    {ArchiveRole::Kai06, 5, "movie/staffroll.mpg", "daybreak06.dat"},
};

bool is_script(const std::string& name) {
    const std::string n = lower_ascii(name);
    return n.rfind("data/scenario/", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0;
}

}  // namespace

const char* role_file_name(ArchiveRole role) {
    for (const auto& k : kKnown)
        if (k.role == role) return k.file;
    return "";
}

ArchiveRole role_of(const Archive& a) {
    for (const auto& k : kKnown)
        if (a.entries().size() == k.entries && a.find(k.member)) return k.role;
    return ArchiveRole::Unknown;
}

bool script_is_ascii(const std::vector<uint8_t>& s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t end = i;
        while (end < s.size() && s[end] != '\n') ++end;
        static const char kVoice[] = "VOICE";
        const bool voice = end - i >= 5 && std::equal(kVoice, kVoice + 5, s.begin() + long(i));
        if (!voice)
            for (size_t k = i; k < end; ++k)
                if (s[k] >= 0x80) return false;
        i = end + 1;
    }
    return true;
}

bool identify_archive(const std::string& path, ArchiveIdent& out, std::string* err) {
    out = ArchiveIdent{};
    Archive a;
    if (!a.open(path, err)) return false;
    out.entries = a.entries().size();
    out.file_size = a.file_size();
    out.role = role_of(a);
    std::vector<uint8_t> data;
    for (const Entry& e : a.entries()) {
        if (!is_script(e.name)) continue;
        if (!a.read(e, data, err)) return false;
        out.scripts++;
        if (script_is_ascii(data)) out.english_scripts++;
    }
    return true;
}

}  // namespace db
