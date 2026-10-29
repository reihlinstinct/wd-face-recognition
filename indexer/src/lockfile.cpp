#include "lockfile.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wfi {

namespace {

// Reads the holder PID from an existing lockfile; 0 when unreadable or
// malformed. Malformed covers a numeric prefix with trailing junk
// ("123junk" must not name pid 123 - CodeRabbit #97) and anything
// outside the pid_t range: the worst case for treating it as stale is
// two instances, the same failure mode as no lock at all.
long read_pid(const std::string& path) {
    char buf[32] = {0};
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    char* end = nullptr;
    errno = 0;
    const long pid = std::strtol(buf, &end, 10);
    // 4194304 is Linux's maximum pid_max (2^22).
    if (end == buf || errno != 0 || pid <= 0 || pid > 4194304) return 0;
    if (*end != '\0' && (*end != '\n' || *(end + 1) != '\0')) return 0;
    return pid;
}

bool pid_alive(long pid) {
    if (kill((pid_t)pid, 0) == 0) return true;
    // EPERM means the process exists but belongs to another user.
    return errno == EPERM;
}

// A crash between the temp create and the link leaves
// "<lock>.tmp.<pid>" behind (reviewer #97): sweep the dead-PID ones
// before acquiring. Live owners' temps stay untouched.
void sweep_stale_temps(const std::string& path) {
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string prefix = base + ".tmp.";
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        const std::string name = de->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        const char* tail = name.c_str() + prefix.size();
        char* end = nullptr;
        const long pid = std::strtol(tail, &end, 10);
        if (end == tail || *end != '\0' || pid <= 0 || pid_alive(pid)) continue;
        unlink((dir + "/" + name).c_str());
    }
    closedir(d);
}

}  // namespace

InstanceLock::~InstanceLock() {
    if (!held_.empty()) unlink(held_.c_str());
}

bool InstanceLock::acquire(const std::string& path, std::string& err) {
    // The holder PID lands atomically: write it to a private temp file,
    // then link(2) it into place. A contender therefore never sees an
    // empty or partial lockfile between create and write (CodeRabbit
    // #97), and link's no-clobber semantics keep the mutual exclusion.
    const std::string tmp =
        path + ".tmp." + std::to_string((long)getpid());
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    const int tfd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (tfd < 0) {
        err = "cannot create the lockfile " + path + ": " +
              std::strerror(errno);
        return false;
    }
    // No short-circuit here (reviewer #97): a failed write must still
    // close the descriptor.
    const bool wrote = write(tfd, buf, (size_t)n) == n;
    const bool closed = close(tfd) == 0;
    if (!wrote || !closed) {
        unlink(tmp.c_str());
        err = "cannot write the lockfile " + path;
        return false;
    }
    sweep_stale_temps(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (link(tmp.c_str(), path.c_str()) == 0) {
            unlink(tmp.c_str());
            held_ = path;
            return true;
        }
        if (errno != EEXIST) {
            unlink(tmp.c_str());
            err = "cannot create the lockfile " + path + ": " +
                  std::strerror(errno);
            return false;
        }
        struct stat before;
        if (stat(path.c_str(), &before) != 0) continue;  // vanished: retry
        const long holder = read_pid(path);
        if (holder > 0 && pid_alive(holder)) {
            unlink(tmp.c_str());
            err = "another instance is already running (pid " +
                  std::to_string(holder) + "), exiting";
            return false;
        }
        // Stale (dead PID, unreadable, or malformed) - but reclaim only
        // the exact file checked above: a fresh live lock that landed in
        // the meantime carries a different inode and must not be
        // unlinked (reviewer #97). The check and the unlink still
        // neighbour each other; the window shrinks to that pair.
        struct stat now;
        if (stat(path.c_str(), &now) != 0) continue;  // vanished: retry
        if (now.st_ino != before.st_ino || now.st_dev != before.st_dev) {
            break;  // replaced: fall through to the polite refusal below
        }
        unlink(path.c_str());
    }
    unlink(tmp.c_str());
    err = "another instance is already running, exiting";
    return false;
}

}  // namespace wfi
