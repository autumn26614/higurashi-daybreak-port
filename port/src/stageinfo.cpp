// stageinfo: print what one Higurashi Daybreak stage loads, following the game's
// Stage::Load (0x405B50) order. Read-only; never modifies the archive.
//
//   stageinfo <daybreak00.dat> <stage>            e.g. stageinfo daybreak00.dat 1
//   stageinfo <daybreak00.dat> <stage> --tree     also print each frame hierarchy
//   stageinfo <daybreak00.dat> --x <entry-name>   one .x file from the archive
//   stageinfo <daybreak00.dat> --stages           list stage directories
//   any form + --overlay <dat>                    search <dat> first (Kai: daybreak04.dat)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "archive.h"
#include "audio.h"
#include "cnv.h"
#include "scenario.h"
#include "geometry.h"
#include "identify.h"
#include "png.h"
#include "stage.h"
#include "xfile.h"

using namespace db;

namespace {

void print_stats(const char* label, const std::string& file, const XFile& x, const Stats& s) {
    std::string extra;
    if (s.anim_sets) extra += "  animsets " + std::to_string(s.anim_sets);
    if (s.skinned) extra += "  skinned " + std::to_string(s.skinned);
    if (s.with_vertex_colors) extra += "  vcolor " + std::to_string(s.with_vertex_colors);
    printf("  %-8s %-22s [%s] frames %d (xform %d)  meshes %d  verts %ld  tris %ld  materials %d%s\n", label,
           utf8(file).c_str(), x.encoding.c_str(), s.frames, s.nonidentity_frames, s.meshes, s.verts, s.tris,
           s.materials, extra.c_str());
    if (!s.world.empty())
        printf("           world bbox (%.0f,%.0f,%.0f)..(%.0f,%.0f,%.0f)\n", s.world.lo[0], s.world.lo[1], s.world.lo[2],
               s.world.hi[0], s.world.hi[1], s.world.hi[2]);
}

int run_stage(const Source& a, int stage, bool tree) {
    char dirbuf[64];
    snprintf(dirbuf, sizeof dirbuf, "data/stage%02d", stage);
    std::string dir = dirbuf;
    printf("Stage %02d (%s)\n", stage, dir.c_str());

    // env.dat: 21 little-endian int32 (Stage::Load 0x405B50)
    if (const Entry* e = a.find(dir + "/env.dat")) {
        std::vector<uint8_t> d;
        std::string err;
        if (a.read(*e, d, &err) && d.size() >= 84) {
            int32_t v[21];
            memcpy(v, d.data(), 84);
            printf("env.dat: fog? start %d end %d colour #%02X%02X%02X | params %d %d %d %d\n", v[0], v[1], v[2] & 255,
                   v[3] & 255, v[4] & 255, v[5], v[6], v[7], v[8]);
            for (int p = 0; p < 4; p++) printf("         spawn %dp (%d, %d, %d)\n", p + 1, v[9 + p * 3], v[10 + p * 3], v[11 + p * 3]);
        }
    } else {
        printf("env.dat: missing\n");
    }

    std::set<std::string> textures;
    long total_tris[2] = {0, 0};
    int total_meshes[2] = {0, 0};
    printf("\nVisual mesh slots 00-07 (game loads \"%%02d.x\" when Stage+0x2C != 0, else \"%%02db.x\"):\n");
    for (int slot = 0; slot < 8; slot++) {
        for (int variant = 0; variant < 2; variant++) {
            char nb[32];
            snprintf(nb, sizeof nb, variant ? "%02db.x" : "%02d.x", slot);
            XFile x;
            std::string err;
            if (!load_x(a, dir + "/" + nb, x, &err)) {
                if (err != "missing") printf("  %-8s %-22s ERROR %s\n", variant ? "" : "slot", nb, err.c_str());
                continue;
            }
            Stats st = analyze_x(x, tree);
            char label[16];
            snprintf(label, sizeof label, "slot %d", slot);
            print_stats(variant ? "" : label, nb, x, st);
            total_tris[variant] += st.tris;
            total_meshes[variant] += st.meshes;
            for (const auto& l : st.tree) printf("           %s\n", l.c_str());
            for (const auto& m : st.mats) {
                if (!m.texture.empty()) textures.insert(m.texture);
                if (!m.effect.empty()) printf("           effect: %s\n", utf8(m.effect).c_str());
            }
        }
    }
    printf("  totals: plain %d meshes / %ld tris;  b-variant %d meshes / %ld tris\n", total_meshes[0], total_tris[0],
           total_meshes[1], total_tris[1]);

    printf("\nCollision meshes (always loaded):\n");
    for (const char* c : {"collision.x", "collision_camera.x", "collision_tama.x"}) {
        XFile x;
        std::string err;
        if (!load_x(a, dir + "/" + c, x, &err)) {
            printf("  %-8s %-22s %s\n", "", c, err.c_str());
            continue;
        }
        Stats st = analyze_x(x, tree);
        print_stats("", c, x, st);
        for (const auto& l : st.tree) printf("           %s\n", l.c_str());
    }

    printf("\nTextures referenced by visual meshes (%zu unique):\n", textures.size());
    int missing = 0;
    for (const auto& t : textures) {
        TexStatus s = resolve_texture(a, dir + "/", t);
        if (!s.found) missing++;
        printf("  %-28s -> %s %s", utf8(t).c_str(), utf8(s.archive_name).c_str(), s.found ? "ok" : "MISSING");
        if (s.mips) printf(" (+%d mip%s)", s.mips, s.mips > 1 ? "s" : "");
        printf("\n");
    }
    printf("  %d missing\n", missing);
    return 0;
}

int run_x(const Source& a, const std::string& name) {
    XFile x;
    std::string err;
    if (!load_x(a, name, x, &err)) {
        fprintf(stderr, "%s: %s\n", name.c_str(), err.c_str());
        return 1;
    }
    Stats st = analyze_x(x, true);
    print_stats("", name, x, st);
    for (const auto& l : st.tree) printf("  %s\n", l.c_str());
    for (size_t i = 0; i < st.mats.size(); i++)
        printf("  material %zu: texture '%s'%s%s\n", i, utf8(st.mats[i].texture).c_str(),
               st.mats[i].effect.empty() ? "" : " effect ", utf8(st.mats[i].effect).c_str());
    return 0;
}

// One .cnv (or the .bmp/.wav name the game asks for) to PNG.
int run_cnv(const Source& a, const std::string& name, const std::string& out_path) {
    const std::string lower = lower_ascii(name);
    const bool is_cnv = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".cnv") == 0;
    const std::string entry = is_cnv ? name : cnv_name(name);
    const Entry* e = a.find(entry);
    std::vector<uint8_t> data;
    std::string err;
    if (!e) {
        fprintf(stderr, "%s: missing\n", entry.c_str());
        return 1;
    }
    if (!a.read(*e, data, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (cnv_kind(data) == CnvKind::Sound) {
        Sound s;
        if (!decode_sound(data, s, &err)) {
            fprintf(stderr, "%s: %s\n", entry.c_str(), err.c_str());
            return 1;
        }
        printf("%s: sound, %u ch, %u Hz, %u bit, %zu PCM bytes (not written)\n", utf8(entry).c_str(), s.channels,
               s.rate, s.bits, s.pcm.size());
        return 0;
    }
    Image img;
    if (!decode_image(data, img, &err)) {
        fprintf(stderr, "%s: %s\n", entry.c_str(), err.c_str());
        return 1;
    }
    if (!write_file(out_path, encode_png_rgba(img.width, img.height, img.rgba), &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("%s: %ux%u (pitch %u), %u bpp -> %s\n", utf8(entry).c_str(), img.width, img.height, img.pitch,
           img.bpp, out_path.c_str());
    return 0;
}

// Decodes every .cnv entry of every archive (shadowed ones included).
int run_cnv_check(const std::vector<Archive>& archives, const std::vector<std::string>& paths) {
    int bad = 0;
    for (size_t i = 0; i < archives.size(); ++i) {
        long images = 0, sounds = 0, bpp24 = 0, bpp32 = 0, errors = 0, other = 0;
        for (const auto& e : archives[i].entries()) {
            std::string n = lower_ascii(e.name);
            if (n.size() < 4 || n.compare(n.size() - 4, 4, ".cnv") != 0) continue;
            std::vector<uint8_t> data;
            std::string err;
            bool ok = archives[i].read(e, data, &err);
            if (ok) switch (cnv_kind(data)) {
                    case CnvKind::Image: {
                        Image img;
                        ok = decode_image(data, img, &err);
                        if (ok) ++images, ++(img.bpp == 32 ? bpp32 : bpp24);
                        break;
                    }
                    case CnvKind::Sound: {
                        Sound s;
                        ok = decode_sound(data, s, &err);
                        if (ok) ++sounds;
                        break;
                    }
                    case CnvKind::Unknown:
                        ++other;
                        ok = false;
                        err = "unrecognised header";
                        break;
                }
            if (!ok && ++errors <= 5) fprintf(stderr, "  %s: %s\n", utf8(e.name).c_str(), err.c_str());
        }
        printf("%s: %ld images (%ld 24-bpp, %ld 32-bpp), %ld sounds, %ld errors (%ld unrecognised)\n",
               paths[i].c_str(), images, bpp24, bpp32, sounds, errors, other);
        bad += errors ? 1 : 0;
    }
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Pull out "--overlay <dat>" pairs; the rest keep their positions.
    std::vector<std::string> overlays, args;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--overlay") == 0 && i + 1 < argc)
            overlays.push_back(argv[++i]);
        else
            args.push_back(argv[i]);
    }
    if (args.size() >= 2 && args[0] == "--identify") {
        int rc = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            ArchiveIdent id;
            std::string e;
            if (!identify_archive(args[i], id, &e)) {
                printf("%s: %s\n", args[i].c_str(), e.c_str());
                rc = 1;
                continue;
            }
            const char* role = role_file_name(id.role);
            printf("%s: %s, %zu entries, %llu bytes, %d scripts, %d English\n", args[i].c_str(),
                   *role ? role : "unknown",
                   id.entries, (unsigned long long)id.file_size, id.scripts, id.english_scripts);
        }
        return rc;
    }
    if (args.size() < 2) {
        fprintf(stderr,
                "usage: %s <daybreak00.dat> <stage> [--tree]\n"
                "       %s <daybreak00.dat> --x <entry-name>\n"
                "       %s <daybreak00.dat> --stages\n"
                "       %s <daybreak00.dat> --cnv <entry-name> <out.png>   (a .bmp name is mapped to .cnv)\n"
                "       %s <daybreak00.dat> --cnv-check                    decode every .cnv entry\n"
                "       %s --identify <dat>...                             which archive; English or not\n"
                "  --overlay <dat>  search this archive before daybreak00.dat (repeatable, in order);\n"
                "                   Kai: --overlay daybreak04.dat\n",
                argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    std::vector<std::string> paths = overlays;
    paths.push_back(args[0]);
    std::vector<Archive> archives(paths.size());
    ArchiveSet set;
    std::string err;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!archives[i].open(paths[i], &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        set.attach(&archives[i]);
    }
    const std::string& arg = args[1];
    if (arg == "--x" && args.size() >= 3) return run_x(set, args[2]);
    if (arg == "--cnv" && args.size() >= 4) return run_cnv(set, args[2], args[3]);
    if (arg == "--cnv-check") return run_cnv_check(archives, paths);
    if (arg == "--wav" && args.size() >= 4) {
        // One sound to a 16-bit WAV file, plus its level.
        Pcm p;
        if (!load_sound(set, args[2], p, &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        double sq = 0;
        int peak = 0;
        for (int16_t s : p.samples) sq += double(s) * s, peak = std::max(peak, std::abs(int(s)));
        std::vector<uint8_t> w;
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) w.push_back(uint8_t(v >> (8 * i))); };
        auto u16 = [&](uint16_t v) { w.push_back(uint8_t(v)), w.push_back(uint8_t(v >> 8)); };
        const uint32_t bytes = uint32_t(p.samples.size() * 2);
        w.insert(w.end(), {'R', 'I', 'F', 'F'}), u32(36 + bytes), w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        u32(16), u16(1), u16(uint16_t(p.channels)), u32(uint32_t(p.rate)), u32(uint32_t(p.rate * p.channels * 2));
        u16(uint16_t(p.channels * 2)), u16(16), w.insert(w.end(), {'d', 'a', 't', 'a'}), u32(bytes);
        for (int16_t s : p.samples) u16(uint16_t(s));
        if (!write_file(args[3], w, &err)) return 1;
        printf("%s: %d ch %d Hz, %zu frames, rms %.0f, peak %d\n", args[2].c_str(), p.channels, p.rate,
               p.samples.size() / size_t(p.channels), std::sqrt(sq / double(std::max<size_t>(1, p.samples.size()))), peak);
        return 0;
    }
    if (arg == "--sounds") {
        // Decode every BGM / effect, and resolve every scenario VOICE line to an entry.
        int ok = 0, bad = 0, looped = 0;
        double seconds = 0;
        for (const auto& ar : archives)
            for (const auto& e : ar.entries()) {
                const std::string low = lower_ascii(e.name);
                const bool bgm = low.rfind("data/bgm/", 0) == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".ogg") == 0;
                const bool se = (low.rfind("data/se/", 0) == 0 || low.rfind("data/sysse/", 0) == 0);
                if (!bgm && !se) continue;
                Pcm p;
                std::string err2;
                if (!load_sound(set, e.name, p, &err2)) {
                    printf("  %s: %s\n", e.name.c_str(), err2.c_str());
                    bad++;
                    continue;
                }
                ok++;
                seconds += double(p.samples.size()) / p.channels / p.rate;
                if (p.loop_start >= 0) looped++;
                if (bgm)
                    printf("  %s: %d ch %d Hz %.1f s, loop %lld..%lld\n", e.name.c_str(), p.channels, p.rate,
                           double(p.samples.size()) / p.channels / p.rate, (long long)p.loop_start, (long long)p.loop_end);
            }
        printf("%d sounds decoded (%.0f s, %d looped), %d failed\n", ok, seconds, looped, bad);
        int voices = 0, found = 0;
        std::vector<uint8_t> d;
        for (const auto& ar : archives)
            for (const auto& e : ar.entries()) {
                const std::string low = lower_ascii(e.name);
                if (low.rfind("data/scenario/", 0) != 0 || low.compare(low.size() - 4, 4, ".txt") != 0) continue;
                std::string err2;
                if (!ar.read(e, d, &err2)) continue;
                const scn::Script s = scn::parse(std::string(d.begin(), d.end()));
                for (const auto& ln : s.lines)
                    if (ln.voice) {
                        voices++;
                        const std::string v = voice_entry(set, ln.voice_file);
                        if (!v.empty()) found++;
                        else if (voices - found <= 5) printf("  voice not found: %s\n", ln.voice_file.c_str());
                    }
            }
        printf("%d VOICE lines, %d resolved\n", voices, found);
        return bad ? 1 : 0;
    }
    if (arg == "--geometry") {
        // Every stage: batches from geometry.cpp against the counting reader's triangles.
        int rc = 0;
        for (int stage = 1; stage <= 99; ++stage) {
            for (int variant = 0; variant < 2; ++variant) {
                StageGeometry g = load_stage_geometry(set, stage, variant);
                if (g.batches.empty()) continue;
                StageSummary s = summarize_stage(set, stage);
                size_t untextured = 0, missing = 0, additive = 0, colored = 0;
                for (const auto& b : g.batches) {
                    untextured += b.mat.texture.empty();
                    missing += !b.mat.texture.empty() && !b.mat.tex.found;
                    additive += b.mat.power != 0;
                    colored += b.has_colors;
                }
                const bool ok = long(g.triangles()) == s.tris[variant] && g.errors.empty();
                printf("stage %02d%s: %zu batches, %zu tris (reader %ld) %s | untextured %zu, texture missing %zu, "
                       "additive %zu, vertex colours %zu | fog %d-%d\n",
                       stage, variant ? "b" : " ", g.batches.size(), g.triangles(), s.tris[variant], ok ? "ok" : "MISMATCH",
                       untextured, missing, additive, colored, g.env.fog_start, g.env.fog_end);
                if (!ok) rc = 1;
            }
        }
        return rc;
    }
    if (arg == "--stages") {
        std::set<std::string> dirs;
        for (const auto& a : archives)
            for (const auto& e : a.entries())
                if (e.name.rfind("data/stage", 0) == 0) dirs.insert(e.name.substr(0, e.name.find('/', 5)));
        for (const auto& d : dirs) printf("%s\n", d.c_str());
        return 0;
    }
    bool tree = args.size() >= 3 && args[2] == "--tree";
    return run_stage(set, atoi(arg.c_str()), tree);
}
