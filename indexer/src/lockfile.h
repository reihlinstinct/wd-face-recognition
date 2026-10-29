// Single-instance guard (issue #46): an O_EXCL lockfile in data_dir so a
// second wdf_indexer/wdf_faces instance exits politely instead of
// interleaving crop writes and face replacements with the first.
#pragma once
#include <string>

namespace wfi {

class InstanceLock {
public:
    InstanceLock() = default;
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    // Unlinks the lockfile if this instance created it.
    ~InstanceLock();

    // Creates path with O_EXCL and writes our PID. An existing file is
    // stale when its PID is dead (kill(pid, 0) == ESRCH) and is then
    // reclaimed; otherwise err names the live holder and false comes back.
    bool acquire(const std::string& path, std::string& err);

private:
    std::string held_;  // empty when nothing is held
};

}  // namespace wfi
