// Headless host for the recompiled daybreak.exe (with d3dx9_25.dll).
//   daybreak-recomp <folder with daybreak.exe>
#include <cstdlib>
#include <string>

#include "host.h"
#include "runtime.h"

#define MODULE_SYMBOLS(P)                                                                    \
    extern const FnEntry P##_table[];                                                        \
    extern const uint32_t P##_count;                                                         \
    extern const ImportSlot P##_imports[];                                                   \
    extern const uint32_t P##_import_count;                                                  \
    extern const ExportSlot P##_exports[];                                                   \
    extern const uint32_t P##_export_count;                                                  \
    extern const uint32_t P##_base;
extern "C" {
MODULE_SYMBOLS(F)
MODULE_SYMBOLS(X)
}
#define MODULE(P, file) \
    rt::Module { file, P##_base, P##_table, P##_count, P##_imports, P##_import_count, P##_exports, P##_export_count }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <folder with daybreak.exe>\n", argv[0]);
        return 2;
    }
    rt::game_root = argv[1];
    static rt::Module exe = MODULE(F, "daybreak.exe");
    static rt::Module d3dx = MODULE(X, "d3dx9_25.dll");
    rt::load_module(rt::game_root + "/daybreak.exe", exe);
    rt::load_module(rt::game_root + "/d3dx9_25.dll", d3dx);
    rt::bind_imports();
    CPU *c = rt::new_cpu(rt::kMainStack);
    rt::attach_dlls(c);
    rt::call_guest(c, exe.entry, {});
    rt::log("recomp: entry point returned (eax=%08x)", c->r[0]);
    return int(c->r[0]);
}
