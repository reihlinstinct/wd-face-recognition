// Single-instance guard (issue #46, reworked in #98): an flock(2) on
// data_dir/wdf.lock so a second wdf_indexer/wdf_faces instance exits
// politely instead of interleaving crop writes and face replacements.
#pragma once
#include <string>

namespace wfi {

class InstanceLock {
public:
    InstanceLock() = default;
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    // Closing the descriptor releases the lock; process death does too.
    ~InstanceLock();

    // Opens path (creating it) and takes an exclusive non-blocking
    // flock. A live holder makes this false with a polite err; I/O
    // failures name the path. The lockfile is never unlinked: a fresh
    // inode would not share the kernel lock.
    bool acquire(const std::string& path, std::string& err);

private:
    int fd_ = -1;
};

}  // namespace wfi
