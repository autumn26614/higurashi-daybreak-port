// Ordered multi-archive lookup (Kai: daybreak04 before daybreak00), checked against two
// small synthetic archives written by this test. No game data is used.
// Usage: archive_set_test <scratch-dir>. Exit status 0 = PASS.
#include <cstdint>
#include <cstdio>
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
// `pad` shifts the payload so the two archives use different per-entry XOR keys.
bool write_archive(const std::string& path, const Files& files, size_t pad) {
    const size_t n = files.size();
    std::vector<uint8_t> index(n * 0x10C, 0), payload(pad, 0xEE);
    uint32_t off = uint32_t(2 + n * 0x10C + pad);
    for (size_t i = 0; i < n; ++i) {
        uint8_t* e = &index[i * 0x10C];
        memcpy(e, files[i].first.data(), files[i].first.size());
        uint32_t size = uint32_t(files[i].second.size());
        memcpy(e + 0x104, &size, 4);
        memcpy(e + 0x108, &off, 4);
        uint8_t k = uint8_t((off >> 1) | 8);
        for (char c : files[i].second) payload.push_back(uint8_t(c) ^ k);
        off += size;
    }
    uint8_t key = 100, step = 100;
    for (auto& b : index) {
        b ^= key;
        key = uint8_t(key + step);
        step = uint8_t(step + 0x4D);
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t cnt[2] = {uint8_t(n), uint8_t(n >> 8)};
    bool ok = fwrite(cnt, 1, 2, f) == 2 && fwrite(index.data(), 1, index.size(), f) == index.size() &&
              fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    return fclose(f) == 0 && ok;
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

std::string read_str(const Source& s, const std::string& name) {
    const Entry* e = s.find(name);
    std::vector<uint8_t> out;
    std::string err;
    if (!e || !s.read(*e, out, &err)) return "<missing>";
    return std::string(out.begin(), out.end());
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

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
