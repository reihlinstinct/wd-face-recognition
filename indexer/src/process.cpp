#include "process.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <net.h>

#include "align.h"
#include "cropreserve.h"
#include "db.h"
#include "detect.h"
#include "jpegio.h"

namespace wfi {

namespace {

// Crops are JPEGs under data_dir/crops; the DB stores the relative path.
// Unlink only ever touches that one directory, and only basenames the DB
// itself recorded (a crop_path with a slash or ".." never reaches unlink).
bool safe_crop_rel(const std::string& rel) {
    return rel.rfind("crops/p", 0) == 0 && rel.find('/') == 5 &&
           rel.find("..") == std::string::npos;
}

}  // namespace

namespace {

// Crop output must be usable BEFORE any model load or queue read
// (CodeRabbit #45): an unwritable data_dir would otherwise fail every
// photo's crop write and mark them all failed, with no automatic retry.
bool ensure_crop_dir(const std::string& data_dir, std::string& err) {
    for (const std::string& dir : {data_dir, data_dir + "/crops"}) {
        // Private face data (review #45): dirs are owner-only.
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            err = "cannot create " + dir + ": " + std::strerror(errno);
            return false;
        }
        struct stat st;
        if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            err = dir + " exists but is not a directory";
            return false;
        }
        if (access(dir.c_str(), W_OK) != 0) {
            err = dir + " is not writable: " + std::strerror(errno);
            return false;
        }
    }
    return true;
}

// mark_failed with the same SQLITE_BUSY retry policy as the commit path:
// a concurrent scan's write lock must not abort the run (review #45).
// Returns false on a real error or persistent contention (check err for
// "locked"); conflict=true means the photo changed under us.
bool mark_failed_with_retry(Db& db, const QueuedPhoto& q, const std::string& why,
                            bool& conflict, std::string& err) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        conflict = false;
        if (db.mark_failed(q.id, q.size, q.mtime, why, conflict, err)) return true;
        if (conflict || err.find("locked") == std::string::npos) return false;
        std::fprintf(stderr, "database busy (scan running?), retry %d/3\n",
                     attempt + 1);
        sleep(1u << attempt);
    }
    return false;
}

}  // namespace

bool process_queued(Db& db, const std::string& models_dir,
                    const std::string& data_dir, int threads, long limit,
                    ProcessCounts& counts, std::string& err) {
    if (!ensure_crop_dir(data_dir, err)) return false;

    ncnn::Net det, rec;
    wfp::load_net(det, models_dir, "scrfd_500m", threads);
    wfp::load_net(rec, models_dir, "mobilefacenet", threads);

    // Per-run crop stamp: replacement crops are written to NEW paths
    // (crops/p<id>_t<stamp>_f<n>.jpg) and the DB rows point at them, so a
    // failed encode or a rolled-back replace_faces never touches bytes the
    // committed rows still reference (CodeRabbit #45). Old paths are
    // unlinked only after the commit.
    const long run_stamp = (long)std::time(nullptr);

    std::vector<QueuedPhoto> queue;
    if (!db.queued_photos(limit, queue, err)) return false;
    for (const QueuedPhoto& q : queue) {
        std::string reason;
        wfp::RgbImage img = wfp::decode_jpeg(q.path, &reason);
        if (img.pixels.empty()) {
            // "too large" for the pixel cap, "decode" for corrupt files.
            const std::string why = reason.empty() ? "decode" : reason;
            bool conflict = false;
            if (!mark_failed_with_retry(db, q, why, conflict, err)) {
                if (conflict) {
                    // Changed under us: leave the newer queue entry alone.
                    ++counts.requeued;
                    continue;
                }
                if (err.find("locked") != std::string::npos) {
                    std::fprintf(stderr,
                        "database stayed busy; stopping, %s stays queued\n",
                        q.path.c_str());
                    long left = db.queued_count();
                    counts.remaining = left < 0 ? limit : left;
                    return true;  // soft stop, not a failure
                }
                err = "mark_failed: " + err;
                return false;
            }
            ++counts.failed;
            std::fprintf(stderr, "skip undecodable %s\n", q.path.c_str());
            continue;
        }
        std::vector<wfp::Face> faces = wfp::scrfd_detect(det, img);
        std::vector<FaceRow> rows;
        std::vector<std::string> new_crops;
        bool crop_failed = false;
        int fi = 0;
        for (const wfp::Face& f : faces) {
            wfp::RgbImage chip = wfp::align_chip(img, f.kps);
            if (chip.pixels.empty()) chip = wfp::crop_chip(img, f.x0, f.y0, f.x1, f.y1);
            if (chip.pixels.empty()) continue;  // degenerate box
            FaceRow r;
            char bbox[64];
            std::snprintf(bbox, sizeof(bbox), "%d,%d,%d,%d",
                          (int)f.x0, (int)f.y0, (int)f.x1, (int)f.y1);
            r.bbox = bbox;
            r.embedding = wfp::arcface_embed(rec, chip);
            // Two concurrent runs in the same second derive the same path
            // (run_stamp has one-second resolution): claim it exclusively
            // BEFORE encoding, so a loser never truncates the winner's
            // crop with "wb" and never unlinks a committed path after its
            // replace_faces version check fails (CodeRabbit #45).
            std::string abs_crop;
            if (!reserve_crop_file(data_dir + "/crops",
                                   "p" + std::to_string(q.id) + "_t" +
                                       std::to_string(run_stamp) + "_f" +
                                       std::to_string(fi),
                                   abs_crop, err)) {
                std::fprintf(stderr, "crop path reservation failed: %s\n",
                             err.c_str());
                crop_failed = true;
                break;
            }
            r.crop_path = "crops" + abs_crop.substr(abs_crop.find_last_of('/'));
            if (!wfp::encode_jpeg(chip.pixels.data(), chip.w, chip.h,
                                  abs_crop)) {
                // Do NOT unlink here: encode_jpeg already removed any
                // partial file, and if it failed at fopen the empty
                // reservation stays - unlinking now could delete a path a
                // concurrent run just claimed after that remove. The
                // unreferenced empty file is reclaimed by the orphan-crop
                // GC (issue #47).
                std::fprintf(stderr, "crop write failed for %s\n",
                             r.crop_path.c_str());
                crop_failed = true;
                break;
            }
            new_crops.push_back(r.crop_path);
            rows.push_back(std::move(r));
            ++fi;
        }
        if (crop_failed) {
            // A photo with partial output must not be recorded as scanned
            // (CodeRabbit #45): mark it failed so the miss is visible, and
            // drop this run's staged crops (never referenced by any row).
            // The photo leaves the queue in state 'failed'; a later scan
            // re-queues it only when the file changes - retrying transient
            // I/O failures is the operator's call (or a future
            // retry-failed command).
            for (const std::string& c : new_crops)
                if (safe_crop_rel(c)) unlink((data_dir + "/" + c).c_str());
            bool conflict = false;
            if (!mark_failed_with_retry(db, q, "crop write", conflict, err)) {
                if (conflict) {
                    ++counts.requeued;
                    continue;
                }
                if (err.find("locked") != std::string::npos) {
                    std::fprintf(stderr,
                        "database stayed busy; stopping, %s stays queued\n",
                        q.path.c_str());
                    long left = db.queued_count();
                    counts.remaining = left < 0 ? limit : left;
                    return true;  // soft stop, not a failure
                }
                err = "mark_failed: " + err;
                return false;
            }
            ++counts.failed;
            continue;
        }
        // Retry on SQLITE_BUSY (a concurrent scan holds the write lock),
        // then soft-stop the run instead of aborting mid-queue: staged
        // crops are removed, the photo stays queued, and the caller is
        // told work remains (review #45).
        std::vector<std::string> old_crops;
        bool conflict = false;
        bool committed = false;
        for (int attempt = 0; attempt < 4 && !committed; ++attempt) {
            conflict = false;
            if (db.replace_faces(q.id, q.size, q.mtime, rows, old_crops,
                                 conflict, err)) {
                committed = true;
            } else if (conflict) {
                break;
            } else if (err.find("locked") != std::string::npos) {
                std::fprintf(stderr, "database busy (scan running?), retry %d/3\n",
                             attempt + 1);
                sleep(1u << attempt);
            } else {
                break;
            }
        }
        if (!committed) {
            // Nothing committed: this run's staged crops are referenced by
            // nothing and always go away. Every path in new_crops was
            // created by THIS run via O_EXCL reservation, so unlinking it
            // can never remove another run's committed crop.
            for (const std::string& c : new_crops)
                if (safe_crop_rel(c)) unlink((data_dir + "/" + c).c_str());
            if (conflict) {
                // A concurrent scan saw the file change and re-queued it:
                // that newer queue entry wins, this run's embeddings are
                // stale. Left queued for the next process run.
                ++counts.requeued;
                std::fprintf(stderr, "%s changed during processing, left queued\n",
                             q.path.c_str());
                continue;
            }
            if (err.find("locked") != std::string::npos) {
                std::fprintf(stderr,
                    "database stayed busy; stopping, %s stays queued\n",
                    q.path.c_str());
                long left = db.queued_count();
                counts.remaining = left < 0 ? limit : left;
                return true;  // soft stop, not a failure
            }
            err = "replace_faces: " + err;
            return false;
        }
        // Committed: stale crops from a previous processing round go away.
        // Only paths the DB itself returned, and never ones just rewritten.
        const std::set<std::string> keep(new_crops.begin(), new_crops.end());
        for (const std::string& c : old_crops)
            if (safe_crop_rel(c) && keep.count(c) == 0)
                unlink((data_dir + "/" + c).c_str());
        ++counts.processed;
        counts.faces += (long)rows.size();
        std::printf("%s: faces=%zu\n", q.path.c_str(), rows.size());
    }
    long remaining = db.queued_count();  // real count, not a one-row probe
    if (remaining < 0) {
        err = "could not count the remaining queue";
        return false;
    }
    counts.remaining = remaining;
    return true;
}

}  // namespace wfi
