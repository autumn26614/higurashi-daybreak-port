// Starting the recompiled game (shared by the headless host and the iOS app). A build may
// hold the base game (daybreak.exe, translated with the prefix F), Kai (DaybreakDX.exe,
// prefix K), or both; d3dx9_25.dll (prefix X) serves either.
#pragma once

#include <string>

namespace rt {

enum class Game { base, kai };

// False when this build was made without that game's translated code (gen_stub.c).
bool game_translated(Game g);
// Its executable: daybreak.exe or DaybreakDX.exe; without an argument, the loaded game's.
std::string game_exe(Game g);
std::string game_exe();
// True if `dir` holds the game: its executable, d3dx9_25.dll and the archives it reads
// (daybreak00-02.dat; Kai also 04-06).
bool game_files_present(const std::string &dir, Game g);
// Maps the modules and binds their imports. Call once, before game_run().
void game_load(const std::string &dir, Game g);
// Runs the game on the calling thread (its stack should be large); returns the exit code.
int game_run();

}  // namespace rt
