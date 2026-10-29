#pragma once
// SQLite index store (docs/INCREMENTAL.md): WAL, secure_delete, batched
// writes. Mark-and-sweep per scan root keeps the walk streaming: no
// whole-library structure in RAM (DESIGN.md bounded-everything).
#include <sqlite3.h>

#include <cstdint>
#include <set>
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

// One stored face with its embedding: the clustering input (PR 3).
struct ClusterFace {
    long id = 0;
    long photo_id = 0;
    std::vector<float> embedding;
};

// A person as the people page lists them (PR 3).
struct PersonSummary {
    long id = 0;
    std::string name;        // empty when unnamed
    long face_count = 0;
    std::string cover_crop;  // crop_path of the cluster's first face
};

// A face as the person page lists it (PR 3).
struct PersonFace {
    long id = 0;
    std::string bbox;
    std::string crop_path;   // relative to data_dir
    std::string photo_path;  // absolute
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
    // Every distinct root the photos table carries, sorted. Used to sweep
    // roots that are no longer configured (root removal, #102).
    bool distinct_roots(std::vector<std::string>& out, std::string& err);
    // Returns false when the update could not be persisted.
    bool finish_job(long job_id, const ScanCounts& c);
    // Up to `limit` photos in state 'queued', oldest first by path.
    // False on a query error: an unreadable queue must fail the run, not
    // silently look empty (CodeRabbit #45).
    bool queued_photos(long limit, std::vector<QueuedPhoto>& out, std::string& err);
    // How many photos are still queued (for progress reporting).
    // -1 on a query error.
    long queued_count();
    // Face rows stored for one photo (-1 on error). Used by tests and the
    // people page.
    long face_count(long photo_id);
    // Every crop_path the faces table references (for orphan-crop GC,
    // issue #47).
    bool all_crop_paths(std::set<std::string>& out, std::string& err);
    // Deletes one photo row; its faces cascade via foreign_keys=ON
    // (issue #48). The deleted faces' crop paths come back in crops for
    // the caller to unlink after the commit (same contract as
    // replace_faces' old_crops); on failure crops is empty.
    bool delete_photo(long id, std::vector<std::string>& crops,
                      std::string& err);
    // Tests only: shorten the lock wait so the busy path is exercisable.
    void set_busy_timeout(int ms);
    // Tests only: fault injection (e.g. sqlite3_progress_handler) needs
    // the live connection.
    sqlite3* handle_for_test() { return db_; }
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

    // --- People (issue #40 PR 3) ---

    // Every stored face with its embedding, oldest first (cluster input).
    // False on a query error.
    bool all_face_embeddings(std::vector<ClusterFace>& out, std::string& err);
    // Rewrites cluster assignments from computed groups in ONE
    // transaction. Id/name preservation is global max-overlap, not group
    // order: the LARGEST fragment of a split cluster keeps its id and
    // name. Faces in no group end with cluster_id NULL; empty groups are
    // skipped (never inserted); duplicate or unknown face ids are
    // rejected before anything is written.
    bool replace_clusters(const std::vector<std::vector<long>>& groups,
                          std::string& err);
    // People page: live clusters, most faces first, cover = first face's
    // crop. False on a query error.
    bool people_list(std::vector<PersonSummary>& out, std::string& err);
    // Person page: one cluster's faces with their photo paths.
    // False on a query error (an unknown id is NOT an error, just empty).
    bool person_faces(long cluster_id, std::vector<PersonFace>& out,
                      std::string& err);
    // Names a live cluster (empty name clears it). False when the id is
    // not a live cluster or the write fails.
    bool rename_cluster(long cluster_id, const std::string& name,
                        std::string& err);
    // Merges `from` into `to`: from's faces move, from is marked
    // merged_into=to and leaves the people list. An unnamed target
    // adopts the source's name; a named target keeps its own. False when
    // either id is not a live cluster, from == to, or the write fails.
    bool merge_clusters(long from, long to, std::string& err);

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
