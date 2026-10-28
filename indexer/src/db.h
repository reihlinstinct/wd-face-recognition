#pragma once
// SQLite index store (docs/INCREMENTAL.md): WAL, secure_delete, batched
// writes. Mark-and-sweep per scan root keeps the walk streaming: no
// whole-library structure in RAM (DESIGN.md bounded-everything).
#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <vector>

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

// A queued photo awaiting the face pipeline (process.cpp). size/mtime are
// the version the queue entry was read at: processing commits only if the
// photo still carries exactly this version (CodeRabbit #45).
struct QueuedPhoto {
    long id = 0;
    std::string path;  // absolute, as the walk observed it
    int64_t size = 0;
    int64_t mtime = 0;
};

// One detected face row to persist for a photo.
struct FaceRow {
    std::string bbox;             // "x0,y0,x1,y1" pixel ints
    std::vector<float> embedding; // raw floats, stored as a BLOB
    std::string crop_path;        // relative to data_dir
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
    // Up to `limit` photos in state 'queued', oldest first by path.
    // False on a query error: an unreadable queue must fail the run, not
    // silently look empty (CodeRabbit #45).
    bool queued_photos(long limit, std::vector<QueuedPhoto>& out, std::string& err);
    // How many photos are still queued (for progress reporting).
    // -1 on a query error.
    long queued_count();
    // Atomically replaces a photo's face rows with the new set and marks
    // the photo 'scanned' - but only if the photo is still queued with the
    // exact size/mtime version the caller processed. A concurrent scan that
    // re-queued the photo (changed file) wins: conflict comes back true,
    // nothing is committed, and the photo stays queued for a later run
    // (CodeRabbit #45). The previous crop paths come back in old_crops for
    // the caller to unlink after the commit (best-effort cleanup).
    bool replace_faces(long photo_id, int64_t size, int64_t mtime,
                       const std::vector<FaceRow>& rows,
                       std::vector<std::string>& old_crops,
                       bool& conflict, std::string& err);
    // Marks a queued photo 'failed' with a reason (decode error etc).
    // Same version guard as replace_faces.
    bool mark_failed(long photo_id, int64_t size, int64_t mtime,
                     const std::string& reason, bool& conflict, std::string& err);
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
