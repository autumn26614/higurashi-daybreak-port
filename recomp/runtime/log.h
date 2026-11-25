// The game's log: every line goes to stderr and, once log_open() is called, to a file that
// survives a crash, with the last lines kept in memory and written out by the crash handler.
#pragma once

#include <string>

namespace rt {

// Opens <dir>/latest.txt (the previous run's becomes previous.txt, and a crash.txt from it
// becomes previous-crash.txt). Creates the folder if needed. Also installs the crash handler.
void log_open(const std::string &dir);

// Writes a crash report (the signal, the program counter, the last lines of the log and the
// host stack) to <dir>/crash.txt. Called by the crash handler; safe to call by hand.
void log_crash_report(const char *why);

}  // namespace rt
