// Prints the object tree of one .x entry: type, name, counts and the first numbers.
// Host-only inspection tool.  xdump <dat> <entry> [max-depth] [max-kids]
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/archive.h"
#include "../src/stage.h"
#include "../src/xfile.h"

using namespace db;

static void dump(const XObject& o, int depth, int max_depth, int max_kids) {
    printf("%*s%s '%s' nums=%zu strs=%zu refs=%zu kids=%zu", depth * 2, "", o.type.c_str(), utf8(o.name).c_str(),
           o.nums.size(), o.strs.size(), o.refs.size(), o.kids.size());
    for (size_t i = 0; i < o.nums.size() && i < 12; i++) printf(i ? ",%g" : " [%g", o.nums[i]);
    if (!o.nums.empty()) printf("]");
    for (size_t i = 0; i < o.strs.size() && i < 2; i++) printf(" \"%s\"", utf8(o.strs[i]).c_str());
    for (size_t i = 0; i < o.refs.size() && i < 2; i++) printf(" {%s}", utf8(o.refs[i]).c_str());
    printf("\n");
    if (depth >= max_depth) return;
    int n = 0;
    for (const auto& k : o.kids) {
        if (n++ >= max_kids) {
            printf("%*s... %zu more\n", depth * 2 + 2, "", o.kids.size() - size_t(max_kids));
            break;
        }
        dump(k, depth + 1, max_depth, max_kids);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    Archive a;
    std::string err;
    XFile x;
    if (!a.open(argv[1], &err) || !load_x(a, argv[2], x, &err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    const int depth = argc > 3 ? atoi(argv[3]) : 2, kids = argc > 4 ? atoi(argv[4]) : 6;
    printf("encoding %s, float %d bits, %zu roots\n", x.encoding.c_str(), x.float_bits, x.roots.size());
    int n = 0;
    for (const auto& r : x.roots) {
        if (n++ >= kids * 3) {
            printf("... %zu more roots\n", x.roots.size() - size_t(kids) * 3);
            break;
        }
        dump(r, 0, depth, kids);
    }
    return 0;
}
