// Tells a user-picked archive apart: which daybreakNN.dat it is, and whether the English
// fan translation has been applied to it.
//
// The header has no magic or version (analysis/02-archive-format.md), so the role comes
// from the entry count plus one entry each archive is known to hold. Counts were measured
// on the retail base archives and on the Kai disc's 04/05/06.
//
// Language: every scenario script (data/scenario/**.txt) in the original 00 and 04 has
// Japanese (non-ASCII) text outside its VOICE lines. The English patch replaces scripts
// with ASCII-only ones (88 of 504 in daybreak00 after the archive-only apply). So
// english_scripts > 0 means at least part of the translation is present.
#pragma once
#include <cstdint>
#include <string>

#include "archive.h"

namespace db {

enum class ArchiveRole : int32_t { Unknown = 0, Base00, Base01, Base02, Kai04, Kai05, Kai06 };

struct ArchiveIdent {
    ArchiveRole role = ArchiveRole::Unknown;
    size_t entries = 0;
    uint64_t file_size = 0;
    int scripts = 0;          // data/scenario/**.txt entries
    int english_scripts = 0;  // of those, with ASCII-only text lines
};

// Canonical file name for a role ("daybreak00.dat"), or "" for Unknown.
const char* role_file_name(ArchiveRole role);

// Role from the index alone.
ArchiveRole role_of(const Archive& a);

// True if no line except VOICE lines has a byte >= 0x80.
bool script_is_ascii(const std::vector<uint8_t>& script);

// Opens (and validates) the archive, then classifies it. Reads every scenario script,
// a few hundred small entries, not the whole file.
bool identify_archive(const std::string& path, ArchiveIdent& out, std::string* err);

}  // namespace db
