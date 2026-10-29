// wdf_faces: face-processing daemon for the index (issue #40 PR 2). Picks
// up what wdf_indexer queued, runs the shared pipeline, and stores faces,
// embeddings and crops. Resource discipline: nice 19 + ionice idle.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <set>
#include <string>
#include <sys/stat.h>

#include "config.h"
#include "db.h"
#include "lockfile.h"
#include "people.h"
#include "process.h"
#include "sysres.h"

#define WFF_VERSION "0.3.0-dev"

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_faces %s\n", WFF_VERSION);
        return 0;
    }
    const std::string cmd = argc >= 2 ? argv[1] : "";
    const bool is_read = cmd == "people" || cmd == "person";
    const bool is_write = cmd == "rename" || cmd == "merge";
    if (argc < 3 || (cmd != "process" && cmd != "cluster" && cmd != "gc" &&
                     !is_read && !is_write)) {
        std::fprintf(stderr,
            "usage: wdf_faces process --config FILE [--models DIR] [--limit N] [--threads N]\n"
            "       wdf_faces cluster --config FILE [--threshold 0.5]\n"
            "       wdf_faces people --config FILE\n"
            "       wdf_faces person --config FILE --id N\n"
            "       wdf_faces rename --config FILE --id N --name NAME\n"
            "       wdf_faces merge --config FILE --from N --to N\n"
            "       wdf_faces gc --config FILE\n");
        return 2;
    }
    const bool is_cluster = cmd == "cluster";
    // Unknown or incomplete options are rejected BEFORE the index is
    // opened: a silently ignored flag must never lead to a full assignment
    // rewrite under the default threshold (issue #68). Each subcommand
    // accepts only its own flags (issue #69).
    wfi::CliOptions opt;
    std::string err;
    if (!wfi::parse_cli_options(argc, argv, 2, cmd.c_str(), opt, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const std::string& config_path = opt.config_path;
    const std::string& models_override = opt.models_override;
    const long limit = opt.limit;
    const int threads_override = opt.threads_override;
    const double threshold = opt.threshold;
    const long id_arg = opt.id_arg;
    const long from_arg = opt.from_arg;
    const long to_arg = opt.to_arg;
    const std::string& name_arg = opt.name_arg;
    wfi::Config cfg;
    if (!wfi::load_config(config_path, cfg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    // Face crops, embeddings and the index are private: never world-readable
    // on a shared NAS (independent review #45).
    umask(0077);
    wfi::renice_self();
    // Mutating runs take the instance lock (issue #46); the read-only
    // people/person JSON commands stay lock-free so the pages still load
    // while a scan or process runs.
    wfi::InstanceLock lock;
    if (!is_read && !lock.acquire(cfg.data_dir + "/wdf.lock", err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    // people/person/rename/merge serve the PHP pages (issue #50 PR 3c):
    // the stripped OS5 PHP has no sqlite extension, so the pages shell
    // out and parse the JSON reads; writes stay commands so auth lives in
    // the page. No models involved.
    if (is_read || is_write) {
        wfi::Db db;
        if (!db.open(cfg.data_dir + "/index.db", err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (cmd == "people") {
            std::vector<wfi::PersonSummary> people;
            if (!db.people_list(people, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            std::printf("%s\n", wfi::people_json(people).c_str());
            return 0;
        }
        if (cmd == "person") {
            if (id_arg < 1) {
                std::fprintf(stderr, "person needs --id N\n");
                return 2;
            }
            std::vector<wfi::PersonSummary> people;
            if (!db.people_list(people, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            wfi::PersonSummary who;
            who.id = id_arg;
            for (const auto& ps : people)
                if (ps.id == id_arg) who = ps;
            std::vector<wfi::PersonFace> faces;
            if (!db.person_faces(id_arg, faces, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            std::printf("%s\n", wfi::person_json(who, faces).c_str());
            return 0;
        }
        if (cmd == "rename") {
            if (id_arg < 1) {
                std::fprintf(stderr, "rename needs --id N\n");
                return 2;
            }
            if (name_arg.empty()) {  // an empty name would silently clear it
                std::fprintf(stderr, "rename needs --name NAME\n");
                return 2;
            }
            if (!db.rename_cluster(id_arg, name_arg, err)) {
                std::fprintf(stderr, "%s\n", err.c_str());
                return 1;
            }
            return 0;
        }
        // merge
        if (from_arg < 1 || to_arg < 1) {
            std::fprintf(stderr, "merge needs --from N --to N\n");
            return 2;
        }
        if (!db.merge_clusters(from_arg, to_arg, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        return 0;
    }

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

    // gc (issue #47): remove crop files the faces table no longer
    // references - crash-window leftovers and pre-fix orphans. Only
    // regular files directly inside data_dir/crops, matched against the
    // DB's own "crops/<name>" paths: nothing outside that directory is
    // ever unlinked (the safe_crop_rel contract from process.cpp).
    if (cmd == "gc") {
        wfi::Db db;
        if (!db.open(cfg.data_dir + "/index.db", err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::set<std::string> keep;
        if (!db.all_crop_paths(keep, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        const std::string dir = cfg.data_dir + "/crops";
        DIR* d = opendir(dir.c_str());
        if (d == nullptr && errno == ENOENT) {
            // No crops yet: nothing to do. Same counts shape as a real
            // sweep (CodeRabbit #99).
            std::printf("removed=0 kept=0 failed=0\n");
            return 0;
        }
        if (d == nullptr) {
            std::fprintf(stderr, "cannot list %s: %s\n", dir.c_str(),
                         std::strerror(errno));
            return 1;
        }
        long removed = 0, kept = 0, failed = 0;
        struct dirent* de;
        errno = 0;
        while ((de = readdir(d)) != nullptr) {
            errno = 0;
            const std::string name = de->d_name;
            if (name == "." || name == ".." ||
                name.find('/') != std::string::npos) continue;
            const std::string full = dir + "/" + name;
            struct stat fst;
            if (lstat(full.c_str(), &fst) != 0 || !S_ISREG(fst.st_mode))
                continue;  // directories and links stay untouched
            if (keep.count("crops/" + name) > 0) {
                ++kept;
                continue;
            }
            if (unlink(full.c_str()) == 0) {
                ++removed;
            } else {
                ++failed;  // counted and named, never silent (review #99)
                std::fprintf(stderr, "gc: cannot remove %s: %s\n",
                             full.c_str(), std::strerror(errno));
            }
        }
        const int read_err = errno;  // readdir ended: 0 = clean EOF
        closedir(d);
        if (read_err != 0) {
            // A partial listing treated as complete would leave orphans
            // behind silently (CodeRabbit #99).
            std::fprintf(stderr, "gc: error reading %s: %s\n", dir.c_str(),
                         std::strerror(read_err));
            return 1;
        }
        std::printf("removed=%ld kept=%ld failed=%ld\n", removed, kept, failed);
        return failed > 0 ? 1 : 0;
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
