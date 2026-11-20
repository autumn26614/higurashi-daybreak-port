// stageinfo: print what one Higurashi Daybreak stage loads, following the game's
// Stage::Load (0x405B50) order. Read-only; never modifies the archive.
//
//   stageinfo <daybreak00.dat> <stage>            e.g. stageinfo daybreak00.dat 1
//   stageinfo <daybreak00.dat> <stage> --tree     also print each frame hierarchy
//   stageinfo <daybreak00.dat> --x <entry-name>   one .x file from the archive
//   stageinfo <daybreak00.dat> --stages           list stage directories
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "archive.h"
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

int run_stage(const Archive& a, int stage, bool tree) {
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

int run_x(const Archive& a, const std::string& name) {
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <daybreak00.dat> <stage> [--tree]\n"
                "       %s <daybreak00.dat> --x <entry-name>\n"
                "       %s <daybreak00.dat> --stages\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    Archive a;
    std::string err;
    if (!a.open(argv[1], &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::string arg = argv[2];
    if (arg == "--x" && argc >= 4) return run_x(a, argv[3]);
    if (arg == "--stages") {
        std::set<std::string> dirs;
        for (const auto& e : a.entries())
            if (e.name.rfind("data/stage", 0) == 0) dirs.insert(e.name.substr(0, e.name.find('/', 5)));
        for (const auto& d : dirs) printf("%s\n", d.c_str());
        return 0;
    }
    bool tree = argc >= 4 && std::string(argv[3]) == "--tree";
    return run_stage(a, atoi(arg.c_str()), tree);
}
