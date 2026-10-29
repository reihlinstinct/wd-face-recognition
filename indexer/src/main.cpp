// wdf_indexer: persistent incremental photo indexer (issue #40, design in
// docs/INCREMENTAL.md). Stat-only mtime-delta walk; detection/embedding is
// a later milestone PR. Resource discipline: nice 19 + ionice idle
// (best-effort), streaming walk, batched DB writes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "config.h"
#include "db.h"
#include "lockfile.h"
#include "sysres.h"
#include "walk.h"

#define WFI_VERSION "0.3.0-dev"

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
    if (cfg.roots.empty()) {
        // Not an error (#108): the walk no-ops, but the run still opens one
        // job and sweeps every stored root below - clearing ALL roots flips
        // every indexed photo to missing, reversibly (#102).
        std::printf("no roots configured - nothing to scan\n");
    }
    // The index holds face data: never world-readable (review #45).
    umask(0077);
    wfi::renice_self();
    // One indexer at a time (issue #46): a second scan exits politely.
    wfi::InstanceLock lock;
    if (!lock.acquire(cfg.data_dir + "/wdf.lock", err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    wfi::Db db;
    if (!db.open(cfg.data_dir + "/index.db", err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // One job generation per RUN, shared by every configured root and by
    // the unconfigured-root sweeps (root removal, #102): sweep_missing
    // marks photos whose last_seen_job differs from the sweeping job, so
    // per-root job ids would mark photos observed earlier in the SAME run
    // as missing. scan_jobs.root records the run's root list (roots never
    // contain commas - rule 6, docs/ROOTS-SETTINGS.md).
    std::string job_roots;
    for (size_t i = 0; i < cfg.roots.size(); ++i) {
        if (i > 0) job_roots += ',';
        job_roots += cfg.roots[i];
    }
    const long job = db.begin_job(job_roots);
    if (job == 0) {
        std::fprintf(stderr, "cannot start scan job\n");
        return 1;
    }

    // Walk every configured root BEFORE any sweep: an overlapping
    // unconfigured stored root's photos may be re-observed under a
    // surviving configured root, and only the shared job stamp exempts
    // them from that root's sweep.
    std::vector<std::string> roots_done;       // canonical, completed walks
    std::vector<std::string> roots_all;        // canonical, every configured root
    std::vector<std::pair<std::string, wfi::ScanCounts>> walked;
    for (const std::string& root : cfg.roots) {
        wfi::ScanCounts c;
        if (!wfi::scan_root(db, job, root, max_depth, c, err)) {
            std::fprintf(stderr, "scan failed under %s: %s\n", root.c_str(), err.c_str());
            return 1;
        }
        walked.push_back({root, c});
        roots_all.push_back(wfi::canonical_root(root));
    }

    wfi::ScanCounts total;
    for (auto& [root, c] : walked) {
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
            roots_done.push_back(wfi::canonical_root(root));
        }
        std::printf("root=%s scanned=%ld new=%ld changed=%ld skipped=%ld missing=%ld restored=%ld\n",
                    root.c_str(), c.scanned, c.added, c.changed, c.skipped, c.missing,
                    c.restored);
        total.scanned += c.scanned; total.added += c.added; total.changed += c.changed;
        total.skipped += c.skipped; total.missing += c.missing;
        total.restored += c.restored;
    }

    // Roots deleted from roots= are never walked, so without this pass
    // their photos would stay scanned (and keep feeding people identities)
    // forever (#102). An unconfigured stored root is swept under the
    // run's shared job only when EVERY overlapping configured root's walk
    // completed: re-observed photos carry the job stamp (unchanged) or
    // adopted the walk's root (changed), so a complete overlapping walk
    // makes the sweep exact, and an incomplete one must suppress it.
    std::vector<std::string> stored;
    if (!db.distinct_roots(stored, err)) {
        std::fprintf(stderr, "cannot list stored roots: %s\n", err.c_str());
        return 1;
    }
    for (const std::string& r : stored) {
        const std::string cr = wfi::canonical_root(r);
        bool configured = false;
        for (const std::string& ca : roots_all) {
            if (ca == cr) { configured = true; break; }
        }
        if (configured) continue;  // swept (or deliberately skipped) above
        bool gate_ok = true;
        for (size_t i = 0; i < roots_all.size(); ++i) {
            if (!wfi::roots_overlap(cr, roots_all[i])) continue;
            bool done = false;
            for (const std::string& cd : roots_done) {
                if (cd == roots_all[i]) { done = true; break; }
            }
            if (!done) { gate_ok = false; break; }
        }
        if (!gate_ok) {
            std::printf("root=%s (unconfigured) sweep suppressed - overlapping walk incomplete\n",
                        r.c_str());
            continue;
        }
        const long swept = db.sweep_missing(job, r);
        if (swept < 0) {
            std::fprintf(stderr, "missing sweep failed for %s\n", r.c_str());
            return 1;
        }
        total.missing += swept;
        if (swept > 0) {
            std::printf("root=%s (unconfigured) missing=%ld\n", r.c_str(), swept);
        }
    }

    if (!db.finish_job(job, total)) {
        std::fprintf(stderr, "could not persist scan job\n");
        return 1;
    }
    std::printf("total scanned=%ld new=%ld changed=%ld skipped=%ld missing=%ld restored=%ld\n",
                total.scanned, total.added, total.changed, total.skipped, total.missing,
                total.restored);
    return 0;
}
