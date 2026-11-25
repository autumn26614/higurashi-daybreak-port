// Starting the recompiled game (shared by the headless host and the iOS app).
#pragma once

#include <string>

namespace rt {

// False in builds made without the translated code (gen_stub.c): nothing to run.
bool game_translated();
// The folder holding daybreak.exe, d3dx9_25.dll and the .dat archives: true if they are all there.
bool game_files_present(const std::string &dir);
// Maps the modules and binds their imports. Call once, before game_run().
void game_load(const std::string &dir);
// Runs the game on the calling thread (its stack should be large); returns the exit code.
int game_run();

}  // namespace rt
