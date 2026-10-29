#include "lockfile.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace wfi {

namespace {

// Reads the holder PID from an existing lockfile; 0 when unreadable or
// malformed (a corrupt file is treated as stale: the worst case is two
// instances, the same failure mode as no lock at all).
long read_pid(const std::string& path) {
    char buf[32] = {0};
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    char* end = nullptr;
    const long pid = std::strtol(buf, &end, 10);
    return (end != buf && pid > 0) ? pid : 0;
}

bool pid_alive(long pid) {
    if (kill((pid_t)pid, 0) == 0) return true;
    // EPERM means the process exists but belongs to another user.
    return errno == EPERM;
}

}  // namespace

InstanceLock::~InstanceLock() {
    if (!held_.empty()) unlink(held_.c_str());
}

bool InstanceLock::acquire(const std::string& path, std::string& err) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            char buf[32];
            const int n = std::snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
            if (write(fd, buf, (size_t)n) != n) {
                close(fd);
                unlink(path.c_str());
                err = "cannot write the lockfile " + path;
                return false;
            }
            close(fd);
            held_ = path;
            return true;
        }
        if (errno != EEXIST) {
            err = "cannot create the lockfile " + path + ": " + std::strerror(errno);
            return false;
        }
        const long holder = read_pid(path);
        if (holder > 0 && pid_alive(holder)) {
            err = "another instance is already running (pid " +
                  std::to_string(holder) + "), exiting";
            return false;
        }
        // Stale (dead PID, unreadable, or corrupt): reclaim and retry once.
        unlink(path.c_str());
    }
    // Lost the reclaim race twice: treat as held.
    err = "another instance is already running, exiting";
    return false;
}

}  // namespace wfi
