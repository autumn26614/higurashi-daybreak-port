#include "host.h"

#include <dirent.h>
#include <sys/stat.h>

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <strings.h>

#include "runtime.h"

namespace rt {

std::string game_root = ".";
namespace {
std::mutex cwd_mutex;
std::string cwd = std::string(kGuestDir) + "\\";
}  // namespace

std::string guest_cwd() {
    std::lock_guard<std::mutex> lock(cwd_mutex);
    return cwd;
}

void set_guest_cwd(const std::string &dir) {
    std::lock_guard<std::mutex> lock(cwd_mutex);
    cwd = dir;
    if (cwd.empty() || cwd.back() != '\\') cwd += '\\';
}

std::string host_path(const std::string &win) {
    std::string p = win;
    for (auto &ch : p)
        if (ch == '/') ch = '\\';
    bool absolute = p.size() >= 2 && p[1] == ':';
    if (!absolute) {
        if (!p.empty() && p[0] == '\\') p = guest_cwd().substr(0, 2) + p;
        else p = guest_cwd() + p;
    }
    std::string dir = std::string(kGuestDir) + "\\";
    if (strncasecmp(p.c_str(), dir.c_str(), dir.size()) != 0) {
        if (strcasecmp(p.c_str(), kGuestDir) == 0) return game_root;
        return "";
    }
    std::string rel = p.substr(dir.size());
    // Walk the components, matching each one without regard to case.
    std::string host = game_root;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('\\', i);
        if (j == std::string::npos) j = rel.size();
        std::string part = rel.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            size_t k = host.find_last_of('/');
            if (k != std::string::npos && host.size() > game_root.size()) host.resize(k);
            continue;
        }
        std::string exact = host + "/" + part;
        struct stat st;
        if (stat(exact.c_str(), &st) == 0) {
            host = exact;
            continue;
        }
        std::string found;
        if (DIR *d = opendir(host.c_str())) {
            while (dirent *e = readdir(d)) {
                if (strcasecmp(e->d_name, part.c_str()) == 0) {
                    found = e->d_name;
                    break;
                }
            }
            closedir(d);
        }
        host += "/" + (found.empty() ? part : found);
    }
    return host;
}

// ---- handles ----
namespace {
std::mutex handle_mutex;
std::map<uint32_t, std::shared_ptr<KObject>> handles;
uint32_t next_handle = 0x1000;
}  // namespace

uint32_t handle_new(std::shared_ptr<KObject> o) {
    std::lock_guard<std::mutex> lock(handle_mutex);
    uint32_t h = next_handle;
    next_handle += 4;
    handles[h] = std::move(o);
    return h;
}

std::shared_ptr<KObject> handle_get(uint32_t h) {
    std::lock_guard<std::mutex> lock(handle_mutex);
    auto it = handles.find(h);
    return it == handles.end() ? nullptr : it->second;
}

bool handle_close(uint32_t h) {
    std::lock_guard<std::mutex> lock(handle_mutex);
    return handles.erase(h) != 0;
}

uint64_t now_us() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return uint64_t(duration_cast<microseconds>(steady_clock::now() - start).count());
}

// ---- printf over guest arguments ----
static std::string wide_to_narrow(uint32_t a) {
    std::string s;
    if (!a) return "(null)";
    for (;; a += 2) {
        uint32_t ch = ld16(a);
        if (!ch) break;
        s += ch < 0x80 ? char(ch) : '?';
    }
    return s;
}

uint32_t format_guest(std::string &out, const char *fmt, uint32_t argp, bool wide) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        std::string spec = "%";
        p++;
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(int32_t(ld32(argp)));
            argp += 4;
            p++;
        }
        while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string(int32_t(ld32(argp)));
                argp += 4;
                p++;
            }
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        int len = 0;  // 0 int, 1 long, 2 64-bit, 3 short, 4 long double
        if (*p == 'h') { len = 3; p++; }
        else if (*p == 'l') { len = 1; p++; if (*p == 'l') { len = 2; p++; } }
        else if (*p == 'L') { len = 4; p++; }
        else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len = 2; p += 3; }
        char conv = *p;
        if (!conv) break;
        char buf[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            if (len == 2) {
                uint64_t v = ld64(argp);
                argp += 8;
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (long long)v);
            } else {
                uint32_t v = ld32(argp);
                argp += 4;
                if (len == 3) v = (conv == 'd' || conv == 'i') ? uint32_t(int16_t(v)) : (v & 0xFFFF);
                snprintf(buf, sizeof buf, (spec + conv).c_str(), (int)v);
            }
            out += buf;
            break;
        }
        case 'c': case 'C': {
            uint32_t v = ld32(argp);
            argp += 4;
            out += char(v);
            break;
        }
        case 's': case 'S': {
            uint32_t a = ld32(argp);
            argp += 4;
            bool w = (conv == 'S') != wide || len == 1;
            if (conv == 's' && len == 3) w = false;
            std::string s = w ? wide_to_narrow(a) : (a ? gstr(a) : "(null)");
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'p': {
            snprintf(buf, sizeof buf, "%08X", ld32(argp));
            argp += 4;
            out += buf;
            break;
        }
        case 'e': case 'E': case 'f': case 'g': case 'G': case 'a': case 'A': {
            double v = ldf64(argp);
            argp += 8;
            snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
            out += buf;
            break;
        }
        case 'n':
            st32(ld32(argp), uint32_t(out.size()));
            argp += 4;
            break;
        case '%':
            out += '%';
            break;
        default:
            out += spec;
            out += conv;
        }
    }
    return argp;
}

}  // namespace rt
