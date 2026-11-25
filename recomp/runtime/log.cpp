#include "log.h"

#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace rt {

namespace {
constexpr size_t kKeep = 400, kLineMax = 400;  // lines kept for the crash report

char ring[kKeep][kLineMax];
std::atomic<unsigned long> ring_count{0};  // lines ever written
std::mutex mu;
FILE *latest = nullptr;
char dir_path[512] = "";

void rename_if(const std::string &from, const std::string &to) {
    struct stat st;
    if (stat(from.c_str(), &st) == 0) rename(from.c_str(), to.c_str());
}

void write_all(int fd, const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= size_t(w);
    }
}

void write_str(int fd, const char *s) { write_all(fd, s, strlen(s)); }

// A message from the crash handler: only async-signal-safe calls (write, open, close).
void crash_handler(int sig, siginfo_t *si, void *uc_) {
    char why[160];
    snprintf(why, sizeof why, "signal %d, fault address %p", sig, si ? si->si_addr : nullptr);
#if defined(__APPLE__)
    if (uc_) {
        ucontext_t *uc = static_cast<ucontext_t *>(uc_);
#if defined(__aarch64__)
        snprintf(why + strlen(why), sizeof why - strlen(why), ", pc %p", (void *)uc->uc_mcontext->__ss.__pc);
#else
        snprintf(why + strlen(why), sizeof why - strlen(why), ", pc %p", (void *)uc->uc_mcontext->__ss.__rip);
#endif
    }
#elif defined(__linux__) && defined(REG_RIP)
    if (uc_) {
        ucontext_t *uc = static_cast<ucontext_t *>(uc_);
        snprintf(why + strlen(why), sizeof why - strlen(why), ", pc %p", (void *)uc->uc_mcontext.gregs[REG_RIP]);
    }
#endif
    log_crash_report(why);
    signal(sig, SIG_DFL);  // SA_NODEFER below: the re-raised signal ends the process
    raise(sig);
}
}  // namespace

void log(const char *fmt, ...) {
    char text[kLineMax];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char line[kLineMax + 64];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld [%04lx] %s", tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000, (unsigned long)(uintptr_t)pthread_self() & 0xFFFF, text);
    fputs(line, stderr);
    fputc('\n', stderr);

    std::lock_guard<std::mutex> lock(mu);
    unsigned long n = ring_count.load();
    strncpy(ring[n % kKeep], line, kLineMax - 1);
    ring[n % kKeep][kLineMax - 1] = 0;
    ring_count.store(n + 1);
    if (latest) {
        fputs(line, latest);
        fputc('\n', latest);
        fflush(latest);
    }
}

void log_open(const std::string &dir) {
    mkdir(dir.c_str(), 0755);
    std::lock_guard<std::mutex> lock(mu);
    snprintf(dir_path, sizeof dir_path, "%s", dir.c_str());
    rename_if(dir + "/crash.txt", dir + "/previous-crash.txt");
    rename_if(dir + "/latest.txt", dir + "/previous.txt");
    if (latest) fclose(latest);
    latest = fopen((dir + "/latest.txt").c_str(), "w");

    struct sigaction sa {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE}) sigaction(sig, &sa, nullptr);
}

void log_crash_report(const char *why) {
    if (!dir_path[0]) return;
    char path[600];
    snprintf(path, sizeof path, "%s/crash.txt", dir_path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    write_str(fd, "Daybreak DX crash report\n\n");
    write_str(fd, why);
    write_str(fd, "\n\nThe last lines of the log:\n");
    unsigned long n = ring_count.load();
    unsigned long first = n > kKeep ? n - kKeep : 0;
    for (unsigned long i = first; i < n; i++) {
        write_str(fd, ring[i % kKeep]);
        write_str(fd, "\n");
    }
    write_str(fd, "\nThe host stack (addresses; the translated functions are named by their guest address):\n");
    void *frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, fd);
    close(fd);
}

}  // namespace rt
