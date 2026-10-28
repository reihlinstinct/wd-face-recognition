#pragma once
// SQLite index store (docs/INCREMENTAL.md): WAL, secure_delete, batched
// writes. Mark-and-sweep per scan root keeps the walk streaming: no
// whole-library structure in RAM (DESIGN.md bounded-everything).
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace wfi {

struct ScanCounts {
    long scanned = 0;   // JPEG files seen under the scanned roots
    long added = 0;     // new paths queued for processing
    long changed = 0;   // size/mtime changed, re-queued
    long skipped = 0;   // unchanged, reused as-is
    long missing = 0;   // previously known, gone from disk now

    bool incomplete = false;    // some dir unreadable: sweep_missing must skip
    bool depth_capped = false;  // walk stopped at max_depth: deeper indexed
                                // paths were not observed, sweep must skip
};

class Db {
public:
    ~Db();
    // Opens (creating) the DB and applies the schema. False on error.
    bool open(const std::string& path, std::string& err);
    // Starts a scan job row, returns its id (0 on error).
    long begin_job(const std::string& root);
    // One photo observation inside a scan. Batches commits internally.
    // Returns 0 skipped (unchanged), 1 added (new), 2 changed (re-queued),
    // or -1 on error.
    int observe(long job_id, const std::string& root, const std::string& path,
                int64_t size, int64_t mtime, std::string& err);
    // Marks rows not seen by this job (per root) as missing; returns the
    // count, or -1 on a DB error.
    long sweep_missing(long job_id, const std::string& root);
    // Returns false when the update could not be persisted.
    bool finish_job(long job_id, const ScanCounts& c);
    // Commits the open batch, if any. Returns false on COMMIT failure
    // (disk full / volume I/O error): the batch is NOT forgotten then, so
    // the caller fails the run instead of printing bogus counts.
    bool flush();

private:
    sqlite3* db_ = nullptr;
    long pending_ = 0;
    bool exec(const char* sql, std::string& err);
    bool ensure_batch(std::string& err);
};

// Model versions stamped on newly queued photos (INCREMENTAL.md section 5).
extern const char* kDetectorVersion;
extern const char* kEmbedderVersion;

}  // namespace wfi
