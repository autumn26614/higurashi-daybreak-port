// Ordered multi-archive lookup (Kai: daybreak04 before daybreak00), checked against two
// small synthetic archives written by this test. No game data is used.
// Usage: archive_set_test <scratch-dir>. Exit status 0 = PASS.
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../src/archive.h"
#include "../src/daybreak_c.h"
#include "../src/stage.h"

using namespace db;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

using Files = std::vector<std::pair<std::string, std::string>>;

// Same layout as port/tools/make_fixture.py (analysis/02-archive-format.md).
// `pad` shifts the payload so archives use different per-entry XOR keys.
std::vector<uint8_t> build_archive(const Files& files, size_t pad, bool xor_index = true) {
    const size_t n = files.size();
    std::vector<uint8_t> index(n * 0x10C, 0), payload(pad, 0xEE);
    uint32_t off = uint32_t(2 + n * 0x10C + pad);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* e = &index[i * 0x10C];
        memcpy(e, files[i].first.data(), std::min<size_t>(files[i].first.size(), 0x104));
        uint32_t size = uint32_t(files[i].second.size());
        for (int b = 0; b < 4; ++b) e[0x104 + b] = uint8_t(size >> (8 * b)), e[0x108 + b] = uint8_t(off >> (8 * b));
        uint8_t k = uint8_t((off >> 1) | 8);
        for (char c : files[i].second) payload.push_back(uint8_t(c) ^ k);
        off += size;
    }
    uint8_t key = 100, step = 100;
    for (auto& b : index) {
        if (xor_index) b ^= key;
        key = uint8_t(key + step);
        step = uint8_t(step + 0x4D);
    }
    std::vector<uint8_t> out = {uint8_t(n), uint8_t(n >> 8)};
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool write_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

bool write_archive(const std::string& path, const Files& files, size_t pad) {
    return write_bytes(path, build_archive(files, pad));
}

std::string read_str(const Source& s, const std::string& name) {
    const Entry* e = s.find(name);
    std::vector<uint8_t> out;
    std::string err;
    if (!e || !s.read(*e, out, &err)) return "<missing>";
    return std::string(out.begin(), out.end());
}

// Re-encrypts one decoded index field in an already built archive (entry i, byte offset
// `at` inside the 0x10C-byte record) so corrupt indexes can be made precisely.
void poke_index(std::vector<uint8_t>& a, size_t i, size_t at, const std::vector<uint8_t>& bytes) {
    uint8_t key = 100, step = 100;
    const size_t start = i * 0x10C + at;
    for (size_t pos = 0; pos < start + bytes.size(); ++pos) {
        if (pos >= start) a[2 + pos] = bytes[pos - start] ^ key;
        key = uint8_t(key + step);
        step = uint8_t(step + 0x4D);
    }
}

std::vector<uint8_t> le32(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }

// Corrupt and edge-case archives: open() must fail cleanly (no crash, no huge allocation)
// or, for the valid edge cases, read the right bytes.
void validation_tests(const std::string& dir) {
    const std::string path = dir + "/corrupt.dat";
    const Files one = {{"data/a.txt", "hello"}};
    std::string err;
    auto opens = [&](const std::vector<uint8_t>& bytes) {
        Archive a;
        err.clear();
        return write_bytes(path, bytes) && a.open(path, &err);
    };
    const std::vector<uint8_t> good = build_archive(one, 0);
    check(opens(good), "valid one-entry archive opens");

    check(!opens({}), "empty file is rejected");
    check(!opens({good.begin(), good.begin() + 2 + 0x10C - 1}), "truncated index is rejected");
    check(!opens({good.begin(), good.end() - 1}) && err.find("outside the file") != std::string::npos,
          "payload one byte short is rejected at open");

    std::vector<uint8_t> huge = good;
    poke_index(huge, 0, 0x104, le32(0xFFFFFFFFu));
    check(!opens(huge), "entry size 0xFFFFFFFF is rejected before any allocation");

    std::vector<uint8_t> past = good;
    poke_index(past, 0, 0x108, le32(uint32_t(good.size())));
    check(!opens(past), "offset at end of file with non-zero size is rejected");

    std::vector<uint8_t> inside = good;
    poke_index(inside, 0, 0x108, le32(10));
    check(!opens(inside), "payload overlapping the index is rejected");

    std::vector<uint8_t> unterminated = good;
    poke_index(unterminated, 0, 0, std::vector<uint8_t>(0x104, 'a'));
    check(!opens(unterminated) && err.find("not terminated") != std::string::npos, "unterminated 0x104-byte name");

    std::vector<uint8_t> noname = good;
    poke_index(noname, 0, 0, {0});
    check(!opens(noname), "empty name is rejected");

    check(!opens(build_archive(one, 0, false)), "index without the rolling XOR (wrong key) is rejected");

    {
        Archive a;
        check(write_bytes(path, build_archive({}, 0)) && a.open(path, &err) && a.entries().empty(),
              "count 0 opens with no entries");
    }
    {
        Archive a;
        check(write_bytes(path, build_archive({{"Data/Dup.txt", "first"}, {"data/dup.txt", "second"}}, 0)) &&
                  a.open(path, &err) && read_str(a, "data/dup.txt") == "first",
              "duplicate names: the first entry wins");
    }
    {
        // Offsets past 64 KiB: the per-entry key ((offset >> 1) | 8) & 0xFF must wrap.
        Archive a;
        const size_t pad = 70001;
        check(write_bytes(path, build_archive({{"data/far.txt", "far away"}}, pad)) && a.open(path, &err) &&
                  a.entries()[0].offset > 0x10000 && read_str(a, "data/far.txt") == "far away",
              "payload beyond 64 KiB decodes");
    }
    {
        // cp932 names: trail byte 0x5C ('\') in 0x95 0x5C, and an ASCII-range trail byte 0x41.
        Archive a;
        const std::string backslash_trail = "data/\x95\x5C.txt", upper_trail = "data/\x83\x41.txt";
        check(write_bytes(path, build_archive({{backslash_trail, "hyou"}, {upper_trail, "a"}}, 0)) &&
                  a.open(path, &err) && read_str(a, backslash_trail) == "hyou" && read_str(a, upper_trail) == "a",
              "cp932 names with 0x5C and 0x41 trail bytes are found");
    }
    {
        Archive a;
        check(!a.open(dir + "/does-not-exist.dat", nullptr), "null err pointer: open failure does not crash");
        std::vector<uint8_t> out;
        Entry e;
        check(!a.read(e, out, nullptr), "null err pointer: read on a closed archive does not crash");
        check(write_bytes(path, good) && a.open(path, nullptr) && a.open(path, nullptr) && read_str(a, "data/a.txt") == "hello",
              "reopening the same Archive works");
    }
}

std::string mesh(int tris) {
    std::string s = "xof 0303txt 0032\nMesh m {\n 4;\n 0.0;0.0;0.0;,\n 1.0;0.0;0.0;,\n 0.0;1.0;0.0;,\n 1.0;1.0;0.0;;\n ";
    s += std::to_string(tris) + ";\n";
    for (int i = 0; i < tris; ++i) s += i ? ",\n 3;1,3,2;" : " 3;0,1,2;";
    s += ";\n MeshMaterialList {\n  1;\n  " + std::to_string(tris) + ";\n";
    for (int i = 0; i < tris; ++i) s += i ? ",\n  0" : "  0";
    s += ";;\n  Material {\n   1.0;1.0;1.0;1.0;;\n   0.0;\n   0.0;0.0;0.0;;\n   0.0;0.0;0.0;;\n"
         "   TextureFilename {\n    \"grass.bmp\";\n   }\n  }\n }\n}\n";
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    const std::string base_path = std::string(argv[1]) + "/set_base.dat";
    const std::string over_path = std::string(argv[1]) + "/set_overlay.dat";
    const Files base = {
        {"data/stage01/env.dat", std::string(84, '\0')},
        {"data/stage01/00.x", mesh(1)},
        {"data/stage01/grass.cnv", "base texture"},
        {"data/shared.txt", "from base"},
        {"data/base_only.txt", "base only"},
    };
    const Files overlay = {
        {"DATA/Stage01/00.X", mesh(2)},  // different case: lookup is case-insensitive
        {"data/shared.txt", "from overlay"},
        {"data/overlay_only.txt", "overlay only"},
    };
    if (!write_archive(base_path, base, 0) || !write_archive(over_path, overlay, 37)) {
        fprintf(stderr, "cannot write test archives under %s\n", argv[1]);
        return 1;
    }

    Archive b, o;
    std::string err;
    check(b.open(base_path, &err) && o.open(over_path, &err), "both archives open");

    ArchiveSet kai;  // Kai order: overlay (daybreak04) first, then base (daybreak00)
    kai.attach(&o);
    kai.attach(&b);
    check(read_str(kai, "data/shared.txt") == "from overlay", "shared name resolves to the first archive");
    check(kai.which("data/shared.txt") == 0, "which() reports the overlay");
    check(read_str(kai, "data/base_only.txt") == "base only", "falls through to the second archive");
    check(kai.which("data/base_only.txt") == 1, "which() reports the base");
    check(read_str(kai, "data/overlay_only.txt") == "overlay only", "overlay-only name");
    check(read_str(kai, "Data/Shared.TXT") == "from overlay", "case-insensitive across the set");
    check(kai.find("data/nothing.txt") == nullptr && kai.which("data/nothing.txt") == -1, "missing name");

    ArchiveSet reversed;  // attach order is the whole rule
    reversed.attach(&b);
    reversed.attach(&o);
    check(read_str(reversed, "data/shared.txt") == "from base", "reversed order flips the winner");

    ArchiveSet only_base;
    only_base.attach(&b);
    std::vector<uint8_t> out;
    check(!only_base.read(*o.find("data/shared.txt"), out, &err), "refuses an entry from another archive");

    StageSummary sb = summarize_stage(b, 1), sk = summarize_stage(kai, 1);
    check(sb.errors.empty() && sk.errors.empty(), "stage parses with no errors");
    check(sb.tris[0] == 1 && sk.tris[0] == 2, "stage mesh comes from the overlay (1 -> 2 tris)");
    check(sk.textures.size() == 1 && sk.textures[0].status.found, "overlay mesh texture resolves from the base");

    const char* paths[] = {over_path.c_str(), base_path.c_str()};
    char cerr[256];
    DBStageReport* r = db_stage_report_open_set(paths, 2, 1, cerr, sizeof cerr);
    check(r && db_stage_triangle_count(r, 0) == 2 && db_stage_texture_resolved_count(r) == 1,
          "C API db_stage_report_open_set uses the same order");
    db_stage_report_free(r);
    DBStageReport* r1 = db_stage_report_open(base_path.c_str(), 1, cerr, sizeof cerr);
    check(r1 && db_stage_triangle_count(r1, 0) == 1, "C API single-archive call unchanged");
    db_stage_report_free(r1);
    check(db_stage_report_open_set(paths, 0, 1, cerr, sizeof cerr) == nullptr, "C API rejects an empty set");

    validation_tests(argv[1]);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
