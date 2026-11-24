// Ordered multi-archive lookup (Kai: daybreak04 before daybreak00), checked against two
// small synthetic archives written by this test. No game data is used.
// Usage: archive_set_test <scratch-dir>. Exit status 0 = PASS.
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../src/archive.h"
#include "../src/daybreak_c.h"
#include "../src/audio.h"
#include "../src/character.h"
#include "../src/cnv.h"
#include "../src/geometry.h"
#include "../src/raster.h"
#include "../src/talk.h"
#include "../src/identify.h"
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


// Archive roles come from entry count + a known member; language from scenario scripts.
// Geometry extraction (src/geometry.h) and the draw recipe, on a hand-written stage.
// Characters (src/character.h): a hand-written skinned model with one matrix-key
// animation; audio (src/audio.h): a PCM .cnv and an .sfl loop; talk backgrounds.
void character_audio_tests(const std::string& dir) {
    // Bone "b" at the origin; a triangle fully weighted to it; the animation moves the
    // bone by +10 in x at tick 80 (one frame at 60 fps).
    const std::string x =
        "xof 0303txt 0032\n"
        "Frame root {\n FrameTransformMatrix { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;; }\n"
        " Frame b { FrameTransformMatrix { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;; } }\n"
        " Mesh m {\n  3;\n  0;0;0;, 1;0;0;, 0;1;0;;\n  1;\n  3;0,1,2;;\n"
        "  MeshMaterialList { 1; 1; 0;; Material { 1;1;1;1;; 0; 0;0;0;; 0.25;0.5;0.75;; } }\n"
        "  SkinWeights { \"b\"; 3; 0,1,2; 1.0,1.0,1.0; 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;; }\n"
        " }\n}\n"
        "AnimationSet walk {\n Animation { {b}\n  AnimationKey { 4; 2;\n"
        "   0; 16; 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;;,\n"
        "   80; 16; 1,0,0,0, 0,1,0,0, 0,0,1,0, 10,0,0,1;;;\n  }\n }\n}\n"
        // A second set after it: D3DX numbers the last set in the file 0.
        "AnimationSet stand {\n Animation { {b}\n  AnimationKey { 4; 1;\n"
        "   0; 16; 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1;;;\n  }\n }\n}\n";
    // PCM sound: WAVEFORMATEX (PCM, mono, 22050 Hz, 16 bit) + data_size + 2 samples.
    std::string snd = {1, 0, 1, 0};
    auto le = [](uint32_t v, int n) { std::string s; for (int i = 0; i < n; i++) s += char(v >> (8 * i)); return s; };
    snd += le(22050, 4) + le(44100, 4) + le(2, 2) + le(16, 2) + le(0, 2) + le(4, 4) + le(1000, 2) + le(uint32_t(-1000) & 0xFFFF, 2);
    // .sfl: RIFF SFPL, cue point at sample 100, ltxt region of 50 samples.
    std::string cue = le(1, 4) + le(1, 4) + le(100, 4) + "data" + le(0, 4) + le(0, 4) + le(100, 4);
    std::string ltxt = le(1, 4) + le(50, 4) + "rgn " + le(0, 2) + le(0, 2) + le(0, 2) + le(0, 2);
    std::string list = "adtl" + std::string("ltxt") + le(uint32_t(ltxt.size()), 4) + ltxt;
    std::string body = "SFPL" + std::string("cue ") + le(uint32_t(cue.size()), 4) + cue + "LIST" + le(uint32_t(list.size()), 4) + list;
    const std::string sfl = "RIFF" + le(uint32_t(body.size()), 4) + body;
    // Two 2x2 opaque "portraits": red and blue.
    auto portrait = [&](uint8_t r, uint8_t b) {
        std::string s = {32};
        s += le(2, 4) + le(2, 4) + le(2, 4) + le(0, 4);
        for (int i = 0; i < 4; i++) s += std::string{char(b), 0, char(r), char(255)};  // BGRA
        return s;
    };
    const std::string path = dir + "/chars.dat";
    Archive a;
    std::string err;
    check(write_archive(path, {{"data/hero/00.X", x}, {"data/se/hit.cnv", snd}, {"data/bgm/t.sfl", sfl},
                               {"data/win/03a.cnv", portrait(255, 0)}, {"data/win/07a.cnv", portrait(0, 255)}},
                        5) &&
              a.open(path, &err),
          "character archive opens");

    Character c;
    check(load_character(a, "data/hero/00.X", c, &err) && c.errors.empty(), "character loads");
    check(c.meshes.size() == 1 && c.meshes[0].skin.size() == 1 && c.anims.size() == 2, "one skinned mesh, two sets");
    check(c.anims.size() == 2 && c.anims[0].name == "stand" && c.anims[1].name == "walk" && c.anims[1].ticks == 80,
          "animation sets numbered as D3DX does: last in the file first");
    {
        const float o[3] = {0, 0, 0};
        float p[3] = {};
        check(bone_point(c, 1, 80.0 / 4800, "B", o, p) && std::abs(p[0] - 10) < 1e-4f && std::abs(p[1]) < 1e-4f,
              "bone point follows the animation (case-insensitive name)");
        check(!bone_point(c, 1, 0, "nobone", o, p), "unknown bone");
    }
    check(list_characters(a) == std::vector<std::string>{"data/hero/00.X"}, "character listed");
    {
        const float o[3] = {0, 0, 0}, ahead[3] = {0, 0, -10}, right[3] = {10, 0, 0};
        check(std::abs(facing_yaw(o, ahead)) < 1e-6f && std::abs(facing_yaw(o, right) + 1.5707964f) < 1e-5f,
              "facing yaw: a model faces -z");
        StageGeometry st;
        const float at[3] = {100, 5, 0};
        place_character(st, pose_character(c, -1, 0), at, facing_yaw(o, right));
        // Vertex (1, 0, 0) turned -90 degrees about y: (x c + z s, , -x s + z c) = (0, 0, 1).
        check(st.batches.size() == 1 && st.batches[0].slot == kCharacterSlot &&
                  std::abs(st.batches[0].positions[3] - 100) < 1e-4f && std::abs(st.batches[0].positions[4] - 5) < 1e-4f &&
                  std::abs(st.batches[0].positions[5] - 1) < 1e-4f,
              "place_character turns, moves and puts the batches in the character slot");
    }
    StageGeometry p0 = pose_character(c, -1, 0), p1 = pose_character(c, 1, 1.0 / 60), ph = pose_character(c, 1, 0.5 / 60);
    check(p0.batches.size() == 1 && p0.batches[0].positions[3] == 1, "bind pose");
    check(p1.batches.size() == 1 && std::abs(p1.batches[0].positions[3] - 11) < 1e-4, "key at tick 80 moves the bone +10");
    check(std::abs(ph.batches[0].positions[3] - 6) < 1e-4, "halfway between keys");
    // Winding (0,1,2) makes the normal +z; the shader's light is (0, 0.287348, 0.957826), so
    // u = 0.958 and, with no toon ramp in this archive (ramp = u), every channel saturates.
    const GBatch& b = p0.batches[0];
    check(b.colors[0] == 255 && b.colors[1] == 255 && b.colors[2] == 255, "toon(N.L) + emissive saturates");
    // Rotate the bone 180 degrees about y: the normal faces -z, u = 0, only the emissive
    // colour remains.
    {
        Character r = c;
        r.anims[1].tracks[0].keys[0] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        const GBatch back = pose_character(r, 1, 0).batches.at(0);
        check(back.colors[0] == 64 && back.colors[1] == 128 && back.colors[2] == 191, "facing away: emissive only");
    }
    {
        Character r = c;
        r.toon = {0.1f, 0.1f, 0.1f, 0.9f, 0.9f, 0.9f};  // two-texel ramp
        r.anims[1].tracks[0].keys[0] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
        const GBatch back = pose_character(r, 1, 0).batches.at(0);
        check(back.colors[0] == int(std::lround((0.1 + 0.25) * 255)), "ramp at u = 0 adds to the emissive colour");
    }
    check(b.has_colors && b.colors[3] == 255, "opaque material alpha");

    Pcm pcm;
    check(load_sound(a, "data/se/hit.wav", pcm, &err) && pcm.channels == 1 && pcm.rate == 22050 &&
              pcm.samples == std::vector<int16_t>{1000, -1000},
          ".wav name maps to the .cnv PCM");
    int64_t start = 0, len = 0;
    const std::vector<uint8_t> sflv(sfl.begin(), sfl.end());
    check(parse_sfl(sflv, start, len) && start == 100 && len == 50, ".sfl cue start and region length");
    check(!parse_sfl(std::vector<uint8_t>(12, 0), start, len), "not an .sfl");
    check(voice_entry(a, "nothing.wav").empty(), "unknown voice resolves to nothing");

    // Win background: portrait 03 at (-32,60) left, 07 at (352,60) right; else black.
    auto load = [](void* ctx, const std::string& name, std::vector<uint8_t>& rgba, int& w, int& h) {
        const Archive& ar = *static_cast<const Archive*>(ctx);
        const Entry* e = ar.find(name);
        std::vector<uint8_t> d;
        Image img;
        std::string e2;
        if (!e || !ar.read(*e, d, &e2) || !decode_image(d, img, &e2)) return false;
        rgba = img.rgba, w = int(img.width), h = int(img.height);
        return true;
    };
    std::vector<uint8_t> bg = talk::background("data/scenario/win/03-07-2.txt", load, &a);
    auto px = [&](int xx, int yy, int ch) { return int(bg[size_t((yy * 800 + xx) * 4 + ch)]); };
    check(px(353, 61, 2) == 255 && px(353, 61, 0) == 0, "right portrait at (352,60)");
    check(px(0, 0, 0) == 0 && px(0, 60, 0) == 0, "left portrait is off screen for a 2x2 image at x=-32");
    bg = talk::background("data/scenario/D60-0.txt", load, &a);
    check(px(353, 61, 2) == 0, "story scripts: black background");
}

void geometry_tests(const std::string& dir) {
    // Slot 01: one quad (fan -> 2 triangles) with material 1 and one triangle with
    // material 0; vertex colours on two vertices; UVs; the material list has fewer
    // indices than faces (the last applies to the rest). Material 1 is additive.
    const std::string x01 =
        "xof 0303txt 0032\n"
        "Mesh quad {\n 5;\n 0;0;0;, 10;0;0;, 10;0;10;, 0;0;10;, 5;5;5;;\n"
        " 2;\n 4;0,1,2,3;, 3;0,1,4;;\n"
        " MeshTextureCoords { 5; 0;0;, 1;0;, 1;1;, 0;1;, 0.5;0.5;; }\n"
        " MeshVertexColors { 2; 0;1.0;0.0;0.0;1.0;, 4;0.0;0.0;1.0;0.5;; }\n"
        " MeshMaterialList {\n  2;\n  1;\n  1;;\n"
        "  Material { 0.5;0.5;0.5;1.0;; 0.0; 0;0;0;; 0;0;0;; TextureFilename { \"grass.bmp\"; } }\n"
        "  Material { 1.0;1.0;1.0;1.0;; 8.0; 0;0;0;; 0;0;0;; }\n"
        " }\n}\n";
    const std::string path = dir + "/geo.dat";
    std::vector<uint8_t> env(84, 0);
    const int32_t env_ints[21] = {100, 900, 0x10, 0x20, 0x130, -10, -20, 30, 40, 1, 2, 3};
    for (int i = 0; i < 21; i++)
        for (int b = 0; b < 4; b++) env[size_t(i * 4 + b)] = uint8_t(uint32_t(env_ints[i]) >> (8 * b));
    Archive a;
    std::string err;
    check(write_archive(path, {{"data/stage07/env.dat", std::string(env.begin(), env.end())},
                               {"data/stage07/01.x", x01},
                               {"data/stage07/grass.cnv", "pixels"}},
                        3) &&
              a.open(path, &err),
          "geometry archive opens");
    const StageGeometry g = load_stage_geometry(a, 7, 0);
    check(g.errors.empty() && g.batches.size() == 1, "partial material list: every face takes material 1");
    if (g.batches.size() == 1) {
        const GBatch& b = g.batches[0];
        check(b.slot == 1 && b.material == 1 && b.indices.size() == 9, "quad fan + triangle = 3 triangles");
        check(b.mat.power == 8.0f && b.mat.texture.empty(), "material power read; no texture on material 1");
        // Fan of 0,1,2,3 is (0,1,2),(0,2,3): original winding kept, vertices compacted.
        const std::vector<uint32_t> want = {0, 1, 2, 0, 2, 3, 0, 1, 4};
        check(b.indices == want, "fan triangulation and winding");
        check(b.positions.size() == 15 && b.positions[3] == 10 && b.positions[14] == 5, "positions");
        check(b.uvs.size() == 10 && b.uvs[2] == 1 && b.uvs[9] == 0.5f, "uvs");
        check(b.has_colors && b.colors[0] == 255 && b.colors[1] == 0 && b.colors[16 + 2] == 255 &&
                  b.colors[16 + 3] == 128 && b.colors[4] == 255,
              "vertex colours (and white where none is given)");
    }
    check(g.env.present && g.env.fog_start == 100 && g.env.fog_end == 900 && g.env.fog_rgb[0] == 0x10 &&
              g.env.fog_rgb[2] == 0x30 && g.env.params[0] == -10 && g.env.spawn[0][0] == 1 && g.env.spawn[0][2] == 3,
          "env.dat fields");
    check(!g.bounds.empty() && g.bounds.hi[0] == 10 && g.bounds.lo[1] == 0, "bounds");

    // Same mesh with an explicit index per face: one batch per material.
    std::string x2 = x01;
    x2.replace(x2.find("  1;\n  1;;"), 10, "  2;\n  1,\n  0;;");
    check(write_archive(path, {{"data/stage07/01.x", x2}, {"data/stage07/grass.cnv", "pixels"}}, 0) &&
              a.open(path, &err),
          "second geometry archive opens");
    const StageGeometry g2 = load_stage_geometry(a, 7, 0);
    check(g2.batches.size() == 2 && g2.batches[0].material == 0 && g2.batches[0].indices.size() == 3 &&
              g2.batches[1].indices.size() == 6 && g2.batches[0].mat.tex.found &&
              g2.batches[0].mat.tex.archive_name == "data/stage07/grass.cnv",
          "per-face materials split into batches; texture resolved in the stage dir");
    check(!g2.env.present, "no env.dat");

    // C API over the same archive.
    const char* p = path.c_str();
    char cerr[256];
    DBArchives* arch = db_archives_open(&p, 1, cerr, sizeof cerr);
    DBStageGeometry* cg = db_geometry_open(arch, 7, 0, cerr, sizeof cerr);
    DBBatch cb{};
    check(cg && db_geometry_batch_count(cg) == 2 && db_geometry_triangle_count(cg) == 3 &&
              db_geometry_batch(cg, 0, &cb) == 1 && cb.vertex_count == 3 && cb.index_count == 3 && cb.texture_found &&
              std::string(cb.texture, size_t(cb.texture_len)) == "data/stage07/grass.cnv" && cb.slot == 1,
          "C API geometry batches");
    db_archives_free(arch);  // geometry outlives the archives
    check(db_geometry_batch(cg, 1, &cb) == 1 && cb.power == 8.0f && cb.indices[5] == 3, "batch data after archives freed");
    check(db_geometry_batch(cg, 2, &cb) == 0 && db_geometry_batch(nullptr, 0, &cb) == 0, "C API batch bounds");
    db_geometry_free(cg);
    check(db_geometry_open(nullptr, 7, 0, cerr, sizeof cerr) == nullptr, "C API rejects null archives");

    // Draw recipe: slot order as Stage::Draw, sky without fog, multiply layer without Z-write.
    DBPass ps{};
    std::string order;
    for (int i = 0; i < db_stage_pass_count(); i++)
        if (db_stage_pass(i, &ps)) order += char('0' + ps.slot);
    check(order == "154362078", "pass order 1 5 4 3 6 2 0 7, then characters (8)");
    check(db_stage_pass(2, &ps) && ps.blend == 1 && !ps.z_write && !ps.fog, "slot 4 multiplies, no Z-write, no fog");
    check(db_stage_pass(6, &ps) && ps.slot == 0 && !ps.fog, "sky has no fog");
    check(db_stage_pass(4, &ps) && ps.alpha_test && ps.point_sampling, "slot 6 alpha-tested, point-sampled");
    check(db_stage_pass(99, &ps) == 0, "pass bounds");
    std::string m3;
    for (int i = 0; i < db_stage_pass_count(); i++)
        if (db_stage_pass(i, &ps) && ps.mode3) m3 += char('0' + ps.slot);
    check(m3 == "15362", "render mode 3 on slots 1 5 3 6 2, not on 4, 0, 7");
    {
        float dark[4] = {0.25f, 0.1f, 0.4f, 0.5f}, add[3] = {9, 9, 9};
        mode3_colors(dark, add);
        check(dark[0] == 0.5f && dark[2] == 0.8f && dark[3] == 0.5f && add[0] == 0 && add[2] == 0,
              "mode 3: dark vertex colours double");
        float bright[4] = {0.75f, 0.5f, 0.25f, 0.5f};
        mode3_colors(bright, add);
        check(bright[0] == 1 && bright[3] == 1 && add[0] == 0.25f && add[1] == 0 && add[2] == -0.25f,
              "mode 3: bright vertex colours add above 0.5");
    }
    check(g.mode3 && !load_stage_geometry(a, 7, 1).mode3, "plain set uses mode 3, b set does not");
}

void identify_tests(const std::string& dir) {
    const std::string path = dir + "/ident.dat";
    auto make = [&](size_t count, const Files& fixed) {
        Files f = fixed;
        for (size_t i = f.size(); i < count; ++i) f.push_back({"data/filler/" + std::to_string(i) + ".bin", "x"});
        return write_bytes(path, build_archive(f, 0));
    };
    auto ident = [&](ArchiveIdent& id) {
        std::string err;
        return identify_archive(path, id, &err);
    };
    ArchiveIdent id;
    const std::string jp = "VOICE a.wav\r\n\x82\xa0\\n\r\n";  // hiragana a, then a literal \n
    const std::string en = "VOICE a.wav\r\nHello\\cFF0000 there\\n\r\n";
    check(make(7677, {{"data/stage01/00.x", "x"}, {"data/scenario/a.txt", jp}, {"data/scenario/win/b.txt", jp}}) &&
              ident(id) && id.role == ArchiveRole::Base00 && id.scripts == 2 && id.english_scripts == 0,
          "7677 entries with stage01 = daybreak00, Japanese");
    check(make(7677, {{"data/stage01/00.x", "x"}, {"data/scenario/a.txt", jp}, {"DATA/Scenario/Win/b.TXT", en}}) &&
              ident(id) && id.role == ArchiveRole::Base00 && id.scripts == 2 && id.english_scripts == 1,
          "one ASCII-only script = English patch present (VOICE lines ignored, case-insensitive path)");
    check(make(7676, {{"data/stage01/00.x", "x"}}) && ident(id) && id.role == ArchiveRole::Unknown,
          "one entry short of daybreak00 is unknown");
    check(make(7677, {{"data/stage02/00.x", "x"}}) && ident(id) && id.role == ArchiveRole::Unknown,
          "right count without the member is unknown");
    check(make(4434, {{"data/stage16/00.x", "x"}}) && ident(id) && id.role == ArchiveRole::Kai04, "daybreak04");
    check(make(37, {{"data/bgm/win.ogg", "x"}}) && ident(id) && id.role == ArchiveRole::Base01, "daybreak01");
    check(make(41, {{"data/bgm/win.ogg", "x"}}) && ident(id) && id.role == ArchiveRole::Kai05, "daybreak05");
    check(make(10, {{"movie/staffroll.mpg", "x"}}) && ident(id) && id.role == ArchiveRole::Base02, "daybreak02");
    check(make(5, {{"movie/staffroll.mpg", "x"}}) && ident(id) && id.role == ArchiveRole::Kai06, "daybreak06");
    check(std::string(role_file_name(ArchiveRole::Kai04)) == "daybreak04.dat" &&
              std::string(role_file_name(ArchiveRole::Unknown)).empty(),
          "role file names");

    DBArchiveInfo info{};
    char cerr[256];
    check(make(4434, {{"data/stage16/00.x", "x"}}) && db_identify_archive(path.c_str(), &info, cerr, sizeof cerr) == 1 &&
              info.role == DB_ROLE_KAI04 && info.entries == 4434 && std::string(db_role_file_name(info.role)) == "daybreak04.dat",
          "C API db_identify_archive");
    check(write_bytes(path, {1, 0}) && db_identify_archive(path.c_str(), &info, cerr, sizeof cerr) == 0 && cerr[0],
          "C API reports a corrupt archive");
    check(db_identify_archive(nullptr, &info, cerr, sizeof cerr) == 0, "C API rejects a null path");
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
    identify_tests(argv[1]);
    geometry_tests(argv[1]);
    character_audio_tests(argv[1]);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
