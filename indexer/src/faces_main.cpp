// wdf_faces: face-processing daemon for the index (issue #40 PR 2). Picks
// up what wdf_indexer queued, runs the shared pipeline, and stores faces,
// embeddings and crops. Resource discipline: nice 19 + ionice idle.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "config.h"
#include "db.h"
#include "people.h"
#include "process.h"
#include "sysres.h"

#define WFF_VERSION "0.3.0-dev"

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_faces %s\n", WFF_VERSION);
        return 0;
    }
    if (argc < 3 || (std::strcmp(argv[1], "process") != 0 &&
                     std::strcmp(argv[1], "cluster") != 0)) {
        std::fprintf(stderr,
            "usage: wdf_faces process --config FILE [--models DIR] [--limit N] [--threads N]\n"
            "       wdf_faces cluster --config FILE [--threshold 0.5]\n");
        return 2;
    }
    const bool is_cluster = std::strcmp(argv[1], "cluster") == 0;
    // Unknown or incomplete options are rejected BEFORE the index is
    // opened: a silently ignored flag must never lead to a full assignment
    // rewrite under the default threshold (issue #68).
    wfi::CliOptions opt;
    std::string err;
    if (!wfi::parse_cli_options(argc, argv, 2, is_cluster, opt, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const std::string& config_path = opt.config_path;
    const std::string& models_override = opt.models_override;
    const long limit = opt.limit;
    const int threads_override = opt.threads_override;
    const double threshold = opt.threshold;
    wfi::Config cfg;
    if (!wfi::load_config(config_path, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    // Face crops, embeddings and the index are private: never world-readable
    // on a shared NAS (independent review #45).
    umask(0077);
    wfi::renice_self();

    // cluster needs no models and no queue cap: it rewrites assignments
    // from stored embeddings only (issue #50).
    if (is_cluster) {
        wfi::Db db;
        if (!db.open(cfg.data_dir + "/index.db", err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        wfi::ClusterCounts cc;
        if (!wfi::recluster(db, threshold, cc, err)) {
            std::fprintf(stderr, "cluster failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("faces=%ld people=%ld singletons=%ld\n",
                    cc.faces, cc.people, cc.singletons);
        return 0;
    }

    const std::string models = models_override.empty() ? cfg.models_dir : models_override;
    if (models.empty()) {
        std::fprintf(stderr, "no models dir: set models_dir in the config or pass --models\n");
        return 2;
    }
    const int threads = threads_override > 0 ? threads_override : cfg.threads;
    if (limit < 1) {  // 0/negative must not become an unbounded run
        std::fprintf(stderr, "--limit must be >= 1\n");
        return 2;
    }
    wfi::Db db;
    if (!db.open(cfg.data_dir + "/index.db", err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    wfi::ProcessCounts c;
    if (!wfi::process_queued(db, models, cfg.data_dir, threads, limit, c, err)) {
        std::fprintf(stderr, "process failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("processed=%ld faces=%ld failed=%ld requeued=%ld remaining=%ld\n",
                c.processed, c.faces, c.failed, c.requeued, c.remaining);
    return 0;
}
