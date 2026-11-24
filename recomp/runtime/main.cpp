// Headless host for the recompiled daybreak.exe.
//   daybreak-recomp <folder with daybreak.exe>
#include <cstdlib>
#include <string>

#include "host.h"
#include "runtime.h"

extern "C" {
extern const FnEntry F_table[];
extern const uint32_t F_count;
extern const ImportSlot F_imports[];
extern const uint32_t F_import_count;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder with daybreak.exe>\n", argv[0]);
        return 2;
    }
    rt::game_root = argv[1];
    rt::Program p{F_table, F_count, F_imports, F_import_count};
    uint32_t entry = rt::load_exe(rt::game_root + "/daybreak.exe", p);
    CPU *c = rt::new_cpu(rt::kMainStack);
    rt::call_guest(c, entry, {});
    rt::log("recomp: entry point returned (eax=%08x)", c->r[0]);
    return int(c->r[0]);
}
