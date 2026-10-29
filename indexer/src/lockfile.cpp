#include "lockfile.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

namespace wfi {

InstanceLock::~InstanceLock() {
    if (fd_ >= 0) close(fd_);
}

bool InstanceLock::acquire(const std::string& path, std::string& err) {
    // One lock per object (review #101): a second acquire would abandon
    // the first descriptor while the kernel still holds its lock until
    // process exit.
    if (fd_ >= 0) {
        err = "this instance already holds the lock";
        return false;
    }
    // flock(2) (issue #98): the lock belongs to the open file
    // description and the kernel drops it on ANY exit, crash included -
    // no stale-PID detection, reclamation, or temp files to get wrong.
    // O_CLOEXEC: a child process must not inherit a share of the lock.
    // Note: flock semantics weaken on NFS/SMB mounts; data_dir lives on
    // the NAS's local volume, where they are solid.
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        err = "cannot open the lockfile " + path + ": " +
              std::strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        close(fd);
        if (e == EWOULDBLOCK) {
            err = "another instance is already running, exiting";
            return false;
        }
        err = "cannot lock " + path + ": " + std::strerror(e);
        return false;
    }
    // Legacy transition guard (issue #105): during the one-time upgrade
    // from the pre-flock build, the old PHP can still launch a
    // pid-file-era mutator, and that scheme neither takes nor respects
    // flock. If the lock file still holds a legacy PID naming a live
    // wdf_* process, refuse politely instead of overlapping it. Stale
    // or foreign content is truncated, ending the transition with the
    // first clean acquire. Residual (documented in the PR): an old
    // process starting between this read and the flock is not seen.
    char buf[32] = {0};
    const ssize_t got = pread(fd, buf, sizeof(buf) - 1, 0);
    if (got > 0) {
        char* end = nullptr;
        const long pid = std::strtol(buf, &end, 10);
        if (end != buf && pid > 0) {
            const bool alive = ::kill(pid, 0) == 0 || errno == EPERM;
            bool wdf = true;  // unreadable comm: refuse conservatively
            if (alive) {
                char procpath[64];
                std::snprintf(procpath, sizeof(procpath),
                              "/proc/%ld/comm", pid);
                char comm[32] = {0};
                const int cfd = open(procpath, O_RDONLY | O_CLOEXEC);
                if (cfd >= 0) {
                    const ssize_t cn = read(cfd, comm, sizeof(comm) - 1);
                    close(cfd);
                    if (cn > 0)
                        wdf = std::strncmp(comm, "wdf_", 4) == 0;
                }
            }
            if (alive && wdf) {
                flock(fd, LOCK_UN);
                close(fd);
                err = "another instance is already running, exiting";
                return false;
            }
        }
        ftruncate(fd, 0);  // stale or foreign: the transition is over
    }
    fd_ = fd;
    return true;
}

}  // namespace wfi
