// Runs every case of cases.c translated (module T, from the 32-bit build) and native
// (n_<name>), on a set of inputs, and reports differences. Floating-point cases may differ
// by a little (x87 extended precision against doubles against SSE): within `slack` units.
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"

extern "C" {
extern const FnEntry T_table[];
extern const uint32_t T_count;
extern const ImportSlot T_imports[];
extern const uint32_t T_import_count;
extern const ExportSlot T_exports[];
extern const uint32_t T_export_count;
extern const uint32_t T_base;
#define NATIVE(name) uint32_t n_##name(uint32_t, uint32_t);
#include "native_list.h"
#undef NATIVE
}

struct Native {
    const char *name;
    uint32_t (*fn)(uint32_t, uint32_t);
};
static const Native kNative[] = {
#define NATIVE(name) {#name, n_##name},
#include "native_list.h"
#undef NATIVE
};

int main(int argc, char **argv) {
    rt::Module m{"cases.exe", T_base, T_table, T_count, T_imports, T_import_count, T_exports, T_export_count};
    rt::load_module(argv[1], m);
    rt::bind_imports();
    CPU *c = rt::new_cpu(rt::kMainStack);
    std::vector<uint32_t> vals = {0, 1, 2, 3, 7, 10, 100, 255, 256, 1000, 65535, 65536, 0x7FFFFFFF, 0x80000000,
                                  0xFFFFFFFF, 0xFFFFFFFE, 0x12345678, 0x9ABCDEF0, 0xDEADBEEF, 31, 33};
    int fails = 0, runs = 0;
    for (const Native &n : kNative) {
        uint32_t addr = 0;
        for (uint32_t i = 0; i < T_export_count; i++)
            if (T_exports[i].name && std::string(T_exports[i].name) == n.name) addr = T_exports[i].addr;
        if (!addr) {
            printf("MISSING %s\n", n.name);
            fails++;
            continue;
        }
        bool fp = strstr(n.name, "float") || strstr(n.name, "double") || strstr(n.name, "fmath") ||
                  strstr(n.name, "fconvert") || strstr(n.name, "structs");
        int case_fails = 0;
        for (uint32_t a : vals)
            for (uint32_t b : vals) {
                uint32_t want = n.fn(a, b);
                uint32_t got = rt::call_guest(c, addr, {a, b});
                runs++;
                int64_t diff = int64_t(int32_t(got)) - int64_t(int32_t(want));
                int64_t mag = int64_t(int32_t(want));
                if (mag < 0) mag = -mag;
                int64_t slack = std::max<int64_t>(2, mag / 1000000);  // a few float ulps
                bool ok = got == want || (fp && diff >= -slack && diff <= slack);
                if (!ok && ++case_fails <= 3)
                    printf("FAIL %s(%08x, %08x) = %08x, native %08x\n", n.name, a, b, got, want);
            }
        if (case_fails) fails++;
        printf("%-12s %s\n", n.name, case_fails ? "FAILED" : "ok");
    }
    printf("%d runs, %d failing cases\n", runs, fails);
    return fails ? 1 : 0;
}
