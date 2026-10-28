// wdf_indexer: persistent incremental photo indexer (issue #40, design in
// docs/INCREMENTAL.md). Stat-only mtime-delta walk; detection/embedding is
// a later milestone PR. Resource discipline: nice 19 + ionice idle
// (best-effort), streaming walk, batched DB writes.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>

#include "config.h"
#include "db.h"
#include "walk.h"

#define WFI_VERSION "0.3.0-dev"

// ionice idle class, best-effort (kernel may deny; harmless).
static void renice_self() {
    setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
    // IOPRIO_WHO_PROCESS=1, IOPRIO_CLASS_IDLE=3 -> (3 << 13)
    syscall(SYS_ioprio_set, 1, 0, (3 << 13) | 0);
#endif
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_indexer %s\n", WFI_VERSION);
        return 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "scan") != 0) {
        std::fprintf(stderr, "usage: wdf_indexer scan --config FILE [--max-depth N]\n");
        return 2;
    }
    std::string config_path;
    int max_depth = 8;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--config") == 0) config_path = argv[i + 1];
        else if (std::strcmp(argv[i], "--max-depth") == 0) max_depth = std::atoi(argv[i + 1]);
    }
    wfi::Config cfg;
    std::string err;
    if (!wfi::load_config(config_path, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    renice_self();
    wfi::Db db;
    if (!db.open(cfg.data_dir + "/index.db", err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    wfi::ScanCounts total;
    for (const std::string& root : cfg.roots) {
        long job = db.begin_job(root);
        if (job == 0) {
            std::fprintf(stderr, "cannot start scan job for %s\n", root.c_str());
            return 1;
        }
        wfi::ScanCounts c;
        if (!wfi::scan_root(db, job, root, max_depth, c, err)) {
            std::fprintf(stderr, "scan failed under %s: %s\n", root.c_str(), err.c_str());
            return 1;
        }
        if (c.incomplete || c.depth_capped) {
            // Coverage is partial (unreadable dirs, or the walk stopped at
            // --max-depth): never sweep - a missing-mark here would be a
            // false positive (CodeRabbit #41).
            std::printf("root=%s INCOMPLETE (%s) - missing sweep skipped\n",
                        root.c_str(),
                        c.incomplete ? "unreadable dir" : "depth cap reached");
        } else {
            long swept = db.sweep_missing(job, root);
            if (swept < 0) {
                std::fprintf(stderr, "missing sweep failed for %s\n", root.c_str());
                return 1;
            }
            c.missing = swept;
        }
        if (!db.finish_job(job, c)) {
            std::fprintf(stderr, "could not persist scan job for %s\n", root.c_str());
            return 1;
        }
        std::printf("root=%s scanned=%ld new=%ld changed=%ld skipped=%ld missing=%ld\n",
                    root.c_str(), c.scanned, c.added, c.changed, c.skipped, c.missing);
        total.scanned += c.scanned; total.added += c.added; total.changed += c.changed;
        total.skipped += c.skipped; total.missing += c.missing;
    }
    std::printf("total scanned=%ld new=%ld changed=%ld skipped=%ld missing=%ld\n",
                total.scanned, total.added, total.changed, total.skipped, total.missing);
    return 0;
}
