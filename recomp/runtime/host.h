// Host services shared by the shims: Windows paths onto the game folder, kernel handles.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rt {

// The folder that holds daybreak.exe; the guest sees it as kGuestDir.
extern std::string game_root;
constexpr const char *kGuestDir = "C:\\Daybreak";
std::string guest_cwd();
void set_guest_cwd(const std::string &dir);
// A guest (Windows) path as a host path, matching names without regard to case. Empty if
// the path is outside the game folder.
std::string host_path(const std::string &win);

// Kernel objects behind HANDLE values.
struct KObject {
    virtual ~KObject() = default;
    virtual bool wait(uint32_t ms) { (void)ms; return true; }  // true when signalled
};
uint32_t handle_new(std::shared_ptr<KObject> o);
std::shared_ptr<KObject> handle_get(uint32_t h);
bool handle_close(uint32_t h);
template <class T> std::shared_ptr<T> handle_as(uint32_t h) { return std::dynamic_pointer_cast<T>(handle_get(h)); }

uint32_t format_guest(std::string &out, const char *fmt, uint32_t argp, bool wide = false);
uint64_t now_us();

}  // namespace rt
