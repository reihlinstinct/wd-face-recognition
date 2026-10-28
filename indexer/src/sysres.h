// Shared resource discipline for the indexer binaries (issue #40):
// nice 19 + ionice idle, best-effort (kernel may deny; harmless).
#pragma once

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace wfi {

inline void renice_self() {
    setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
    // IOPRIO_WHO_PROCESS=1, IOPRIO_CLASS_IDLE=3 -> (3 << 13)
    syscall(SYS_ioprio_set, 1, 0, (3 << 13) | 0);
#endif
}

}  // namespace wfi
