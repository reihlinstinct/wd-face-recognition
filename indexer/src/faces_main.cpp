// wdf_faces: face-processing daemon for the index (issue #40 PR 2). Picks
// up what wdf_indexer queued, runs the shared pipeline, and stores faces,
// embeddings and crops. Resource discipline: nice 19 + ionice idle.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "config.h"
#include "db.h"
#include "process.h"
#include "sysres.h"

#define WFF_VERSION "0.3.0-dev"

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_faces %s\n", WFF_VERSION);
        return 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "process") != 0) {
        std::fprintf(stderr,
            "usage: wdf_faces process --config FILE [--models DIR] [--limit N] [--threads N]\n");
        return 2;
    }
    std::string config_path, models_override;
    long limit = 500;  // per-invocation cap: a runaway library cannot hog the NAS
    int threads_override = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--config") == 0) config_path = argv[i + 1];
        else if (std::strcmp(argv[i], "--models") == 0) models_override = argv[i + 1];
        else if (std::strcmp(argv[i], "--limit") == 0) limit = std::atol(argv[i + 1]);
        else if (std::strcmp(argv[i], "--threads") == 0) threads_override = std::atoi(argv[i + 1]);
    }
    wfi::Config cfg;
    std::string err;
    if (!wfi::load_config(config_path, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const std::string models = models_override.empty() ? cfg.models_dir : models_override;
    if (models.empty()) {
        std::fprintf(stderr, "no models dir: set models_dir in the config or pass --models\n");
        return 2;
    }
    const int threads = threads_override > 0 ? threads_override : cfg.threads;
    wfi::renice_self();
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
