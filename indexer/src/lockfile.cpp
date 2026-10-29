#include "lockfile.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace wfi {

InstanceLock::~InstanceLock() {
    if (fd_ >= 0) close(fd_);
}

bool InstanceLock::acquire(const std::string& path, std::string& err) {
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
    fd_ = fd;
    return true;
}

}  // namespace wfi
