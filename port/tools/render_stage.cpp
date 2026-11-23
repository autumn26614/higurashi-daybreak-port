// CPU reference render of one stage to a PNG (src/raster.h): what the iOS app's Metal
// renderer should show. Host-only inspection tool.
//
//   render_stage <daybreak00.dat> <stage> <out.png> [--overlay <dat>]... [--variant b]
//                [--size WxH] [--eye x,y,z --target x,y,z]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/archive.h"
#include "../src/camera.h"
#include "../src/geometry.h"
#include "../src/png.h"
#include "../src/raster.h"

using namespace db;

namespace {
bool parse3(const char* s, float v[3]) { return sscanf(s, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3; }
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> overlays, args;
    int variant = 0, W = 960, H = 540;
    float eye[3], target[3];
    bool have_eye = false, have_target = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--overlay") && i + 1 < argc) overlays.push_back(argv[++i]);
        else if (!strcmp(argv[i], "--variant") && i + 1 < argc) variant = argv[++i][0] == 'b';
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--eye") && i + 1 < argc) have_eye = parse3(argv[++i], eye);
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) have_target = parse3(argv[++i], target);
        else args.push_back(argv[i]);
    }
    if (args.size() != 3 || W <= 0 || H <= 0) {
        fprintf(stderr, "usage: %s <daybreak00.dat> <stage> <out.png> [--overlay <dat>]... [--variant b] "
                        "[--size WxH] [--eye x,y,z --target x,y,z]\n", argv[0]);
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
    const StageGeometry g = load_stage_geometry(set, atoi(args[1].c_str()), variant);
    if (g.batches.empty()) {
        fprintf(stderr, "stage has no meshes\n");
        return 1;
    }
    Camera cam = default_camera(g);
    if (have_eye) std::copy(eye, eye + 3, cam.eye);
    if (have_target) std::copy(target, target + 3, cam.target);
    std::vector<uint8_t> out;
    const long drawn = render_reference(set, g, cam, W, H, out);
    if (!write_file(args[2], encode_png_rgba(uint32_t(W), uint32_t(H), out), &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    printf("stage %02d%s: %zu batches, %zu tris, %ld pixels drawn, eye (%.0f, %.0f, %.0f) -> (%.0f, %.0f, %.0f)\n",
           g.stage, variant ? "b" : "", g.batches.size(), g.triangles(), drawn, cam.eye[0], cam.eye[1], cam.eye[2],
           cam.target[0], cam.target[1], cam.target[2]);
    return 0;
}
