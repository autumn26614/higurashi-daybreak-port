#include "game.h"

#include <sys/stat.h>

#include <vector>

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
MODULE_SYMBOLS(K)
MODULE_SYMBOLS(X)
}
#define MODULE(P, file) \
    rt::Module { file, P##_base, P##_table, P##_count, P##_imports, P##_import_count, P##_exports, P##_export_count }

namespace rt {

namespace {
Module &base_exe() {
    static Module m = MODULE(F, "daybreak.exe");
    return m;
}
Module &kai_exe() {
    static Module m = MODULE(K, "DaybreakDX.exe");
    return m;
}
Module &d3dx() {
    static Module m = MODULE(X, "d3dx9_25.dll");
    return m;
}
Module *loaded = nullptr;
Game loaded_game = Game::base;
}  // namespace

bool game_translated(Game g) { return (g == Game::kai ? K_count : F_count) > 0 && X_count > 0; }

std::string game_exe(Game g) { return g == Game::kai ? "DaybreakDX.exe" : "daybreak.exe"; }

std::string game_exe() { return game_exe(loaded_game); }

bool game_files_present(const std::string &dir, Game g) {
    struct stat st;
    std::vector<std::string> files = {game_exe(g), "d3dx9_25.dll", "daybreak00.dat", "daybreak01.dat", "daybreak02.dat"};
    if (g == Game::kai)
        for (const char *f : {"daybreak04.dat", "daybreak05.dat", "daybreak06.dat"}) files.push_back(f);
    for (auto &f : files)
        if (stat((dir + "/" + f).c_str(), &st) != 0) return false;
    return true;
}

void game_load(const std::string &dir, Game g) {
    game_root = dir;
    loaded = g == Game::kai ? &kai_exe() : &base_exe();
    loaded_game = g;
    load_module(dir + "/" + game_exe(g), *loaded);
    load_module(dir + "/d3dx9_25.dll", d3dx());
    bind_imports();
}

int game_run() {
    if (!loaded) fatal("game_run before game_load");
    CPU *c = new_cpu(kMainStack);
    attach_dlls(c);
    call_guest(c, loaded->entry, {});
    log("recomp: entry point returned (eax=%08x)", c->r[0]);
    return int(c->r[0]);
}

}  // namespace rt
