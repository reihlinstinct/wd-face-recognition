#include "db.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <sys/stat.h>

namespace wfi {

const char* kDetectorVersion = "scrfd-500m-int8";
const char* kEmbedderVersion = "mobilefacenet-int8";

namespace {

const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA secure_delete=ON;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS photos(
  id INTEGER PRIMARY KEY,
  root TEXT NOT NULL,
  path TEXT NOT NULL UNIQUE,
  size INTEGER NOT NULL,
  mtime INTEGER NOT NULL,
  content_hash BLOB,
  state TEXT NOT NULL,
  scanned_at INTEGER,
  fail_reason TEXT,
  detector_ver TEXT NOT NULL,
  embedder_ver TEXT NOT NULL,
  last_seen_job INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_photos_root_seen ON photos(root, last_seen_job);
CREATE TABLE IF NOT EXISTS faces(
  id INTEGER PRIMARY KEY,
  photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE,
  bbox TEXT NOT NULL,
  embedding BLOB NOT NULL,
  crop_path TEXT NOT NULL,
  cluster_id INTEGER REFERENCES clusters(id)
);
CREATE INDEX IF NOT EXISTS idx_faces_cluster ON faces(cluster_id);
CREATE TABLE IF NOT EXISTS clusters(
  id INTEGER PRIMARY KEY,
  name TEXT,
  merged_into INTEGER
);
CREATE TABLE IF NOT EXISTS scan_jobs(
  id INTEGER PRIMARY KEY,
  started INTEGER NOT NULL,
  finished INTEGER,
  root TEXT NOT NULL,
  scanned INTEGER, added INTEGER, changed INTEGER, skipped INTEGER, missing INTEGER
);
INSERT OR IGNORE INTO meta(key, value) VALUES('schema_version', '1');
)SQL";

}  // namespace

Db::~Db() {
    if (db_) {
        (void)flush();  // best-effort at close; run already reported failures
        sqlite3_close(db_);
    }
}

bool Db::exec(const char* sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

bool Db::open(const std::string& path, std::string& err) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err = "cannot open db: " + path;
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    if (!exec(kSchema, err)) return false;
    // Face data is private (review #45): owner-only, even if an earlier
    // version created the file under a loose umask. Best-effort: a
    // filesystem without chmod (FAT USB share) must not block the index.
    (void)chmod(path.c_str(), 0600);
    return true;
}

void Db::set_busy_timeout(int ms) { sqlite3_busy_timeout(db_, ms); }

long Db::face_count(long photo_id) {
    (void)flush();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT count(*) FROM faces WHERE photo_id = ?",
                           -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, photo_id);
    long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

long Db::begin_job(const std::string& root) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO scan_jobs(started, root) VALUES(strftime('%s','now'), ?)",
            -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, root.c_str(), -1, SQLITE_TRANSIENT);
    long id = 0;
    if (sqlite3_step(st) == SQLITE_DONE) id = (long)sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(st);
    return id;
}

bool Db::ensure_batch(std::string& err) {
    if (pending_ > 0) return true;
    char* msg = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "BEGIN failed";
        sqlite3_free(msg);
        return false;
    }
    return true;
}

bool Db::flush() {
    if (pending_ == 0) return true;
    char* msg = nullptr;
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &msg) != SQLITE_OK) {
        // Keep pending_ set: the caller must fail the run; a later flush can
        // still retry the COMMIT before the connection closes.
        sqlite3_free(msg);
        return false;
    }
    pending_ = 0;
    return true;
}

int Db::observe(long job_id, const std::string& root, const std::string& path,
                int64_t size, int64_t mtime, std::string& err) {
    // Batched writes (INCREMENTAL.md section 2): commit every 100 photos.
    if (!ensure_batch(err)) return -1;
    ++pending_;
    if (pending_ >= 100 && !flush()) {
        err = "COMMIT failed (disk full or volume I/O error?)";
        return -1;
    }

    // Read-then-write (SQLite 3.34 on the NAS has no RETURNING): the prior
    // row decides whether this observation is new / changed / unchanged.
    sqlite3_stmt* q = nullptr;
    const char* sel = "SELECT size, mtime, state FROM photos WHERE path = ?";
    if (sqlite3_prepare_v2(db_, sel, -1, &q, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return -1;
    }
    sqlite3_bind_text(q, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(q);
    bool existed = (rc == SQLITE_ROW);
    int64_t old_size = existed ? sqlite3_column_int64(q, 0) : 0;
    int64_t old_mtime = existed ? sqlite3_column_int64(q, 1) : 0;
    std::string old_state = existed
        ? std::string((const char*)sqlite3_column_text(q, 2)) : "";
    sqlite3_finalize(q);
    if (!existed && rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        return -1;
    }

    int outcome;  // 0 skipped, 1 added, 2 changed
    if (!existed) {
        outcome = 1;
    } else if (old_size != size || old_mtime != mtime || old_state == "missing") {
        outcome = 2;
    } else {
        outcome = 0;
    }

    sqlite3_stmt* st = nullptr;
    bool ok;
    if (outcome == 0) {
        const char* sql = "UPDATE photos SET last_seen_job = ? WHERE path = ?";
        ok = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(st, 1, job_id);
            sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        }
    } else {
        // New or changed (or back from missing): (re)queue for processing,
        // clearing the previous scan stamp so the pipeline redoes it.
        const char* sql =
            "INSERT INTO photos(root, path, size, mtime, state, detector_ver,"
            "                   embedder_ver, last_seen_job)"
            " VALUES(?, ?, ?, ?, 'queued', ?, ?, ?)"
            " ON CONFLICT(path) DO UPDATE SET"
            "   root = excluded.root, size = excluded.size, mtime = excluded.mtime,"
            "   state = 'queued', scanned_at = NULL, fail_reason = NULL,"
            "   detector_ver = excluded.detector_ver,"
            "   embedder_ver = excluded.embedder_ver,"
            "   last_seen_job = excluded.last_seen_job";
        ok = sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(st, 1, root.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 3, size);
            sqlite3_bind_int64(st, 4, mtime);
            sqlite3_bind_text(st, 5, kDetectorVersion, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 6, kEmbedderVersion, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 7, job_id);
        }
    }
    if (ok) {
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) err = sqlite3_errmsg(db_);
    } else {
        err = sqlite3_errmsg(db_);
    }
    if (st) sqlite3_finalize(st);
    return ok ? outcome : -1;
}

long Db::sweep_missing(long job_id, const std::string& root) {
    if (!flush()) return -1;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE photos SET state='missing'"
        " WHERE root = ? AND last_seen_job != ? AND state != 'missing'";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, root.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, job_id);
    long n = 0;
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok) n = (long)sqlite3_changes(db_);
    sqlite3_finalize(st);
    return ok ? n : -1;  // CodeRabbit #41: a failed step is not "0 swept"
}

bool Db::finish_job(long job_id, const ScanCounts& c) {
    if (!flush()) return false;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE scan_jobs SET finished=strftime('%s','now'), scanned=?, added=?,"
        " changed=?, skipped=?, missing=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, c.scanned);
    sqlite3_bind_int64(st, 2, c.added);
    sqlite3_bind_int64(st, 3, c.changed);
    sqlite3_bind_int64(st, 4, c.skipped);
    sqlite3_bind_int64(st, 5, c.missing);
    sqlite3_bind_int64(st, 6, job_id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool Db::queued_photos(long limit, std::vector<QueuedPhoto>& out, std::string& err) {
    out.clear();
    if (!flush()) {  // a pending observe batch must be visible to this read
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id, path, size, mtime FROM photos"
        " WHERE state = 'queued' ORDER BY path LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, limit);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        QueuedPhoto q;
        q.id = sqlite3_column_int64(st, 0);
        q.path = (const char*)sqlite3_column_text(st, 1);
        q.size = sqlite3_column_int64(st, 2);
        q.mtime = sqlite3_column_int64(st, 3);
        out.push_back(std::move(q));
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        err = sqlite3_errmsg(db_);
        out.clear();
        return false;
    }
    return true;
}

long Db::queued_count() {
    (void)flush();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT count(*) FROM photos WHERE state = 'queued'",
                           -1, &st, nullptr) != SQLITE_OK) return -1;
    long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool Db::replace_faces(long photo_id, int64_t size, int64_t mtime,
                       const std::vector<FaceRow>& rows,
                       std::vector<std::string>& old_crops,
                       bool& conflict, std::string& err) {
    conflict = false;
    err.clear();  // no stale content: a fresh failure must set it (CodeRabbit #45)
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    bool ok = false;
    do {
        // Collect the old crop paths first: the caller unlinks them only
        // after this transaction commits, so a failed replace never leaves
        // the DB pointing at deleted files.
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT crop_path FROM faces WHERE photo_id = ?",
                               -1, &q, nullptr) != SQLITE_OK) break;
        sqlite3_bind_int64(q, 1, photo_id);
        while (sqlite3_step(q) == SQLITE_ROW)
            old_crops.emplace_back((const char*)sqlite3_column_text(q, 0));
        sqlite3_finalize(q);

        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM faces WHERE photo_id = ?",
                               -1, &d, nullptr) != SQLITE_OK) break;
        sqlite3_bind_int64(d, 1, photo_id);
        ok = sqlite3_step(d) == SQLITE_DONE;
        sqlite3_finalize(d);
        if (!ok) break;

        for (const FaceRow& r : rows) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO faces(photo_id, bbox, embedding, crop_path)"
                    " VALUES(?, ?, ?, ?)", -1, &st, nullptr) != SQLITE_OK) {
                ok = false;
                break;
            }
            sqlite3_bind_int64(st, 1, photo_id);
            sqlite3_bind_text(st, 2, r.bbox.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(st, 3, r.embedding.data(),
                              (int)(r.embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, r.crop_path.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) break;
        }
        if (!ok) break;

        // Version guard (CodeRabbit #45): commit only if the photo is
        // still queued at exactly the version we processed. A concurrent
        // scan that saw the file change re-queued it with new size/mtime;
        // clobbering that would leave stale embeddings marked 'scanned'.
        sqlite3_stmt* u = nullptr;
        if (sqlite3_prepare_v2(db_,
                "UPDATE photos SET state = 'scanned', scanned_at = strftime('%s','now'),"
                " fail_reason = NULL"
                " WHERE id = ? AND state = 'queued' AND size = ? AND mtime = ?",
                -1, &u, nullptr) != SQLITE_OK) {
            ok = false;
            break;
        }
        sqlite3_bind_int64(u, 1, photo_id);
        sqlite3_bind_int64(u, 2, size);
        sqlite3_bind_int64(u, 3, mtime);
        ok = sqlite3_step(u) == SQLITE_DONE;
        const bool version_match = ok && sqlite3_changes(db_) == 1;
        sqlite3_finalize(u);
        if (ok && !version_match) {
            conflict = true;
            ok = false;
        }
    } while (false);

    if (ok) ok = exec("COMMIT", err);
    if (!ok) {
        // Capture SQLite's diagnostic BEFORE the rollback: ROLLBACK itself
        // can overwrite the connection's error state (CodeRabbit #45).
        if (!conflict && err.empty()) err = sqlite3_errmsg(db_);
        std::string rb;
        exec("ROLLBACK", rb);  // best-effort; original error is the one to keep
        old_crops.clear();  // nothing was committed: nothing may be unlinked
    }
    return ok;
}

bool Db::mark_failed(long photo_id, int64_t size, int64_t mtime,
                     const std::string& reason, bool& conflict, std::string& err) {
    conflict = false;
    err.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE photos SET state = 'failed', fail_reason = ?,"
            " scanned_at = strftime('%s','now')"
            " WHERE id = ? AND state = 'queued' AND size = ? AND mtime = ?",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, photo_id);
    sqlite3_bind_int64(st, 3, size);
    sqlite3_bind_int64(st, 4, mtime);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok && sqlite3_changes(db_) != 1) {
        // Changed under us (or dequeued): leave the newer queue state alone.
        conflict = true;
    } else if (!ok) {
        err = sqlite3_errmsg(db_);
    }
    sqlite3_finalize(st);
    return ok && !conflict;
}

bool Db::all_face_embeddings(std::vector<ClusterFace>& out, std::string& err) {
    out.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, photo_id, embedding FROM faces ORDER BY id",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    while (true) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            err = sqlite3_errmsg(db_);
            out.clear();
            sqlite3_finalize(st);
            return false;
        }
        ClusterFace f;
        f.id = sqlite3_column_int64(st, 0);
        f.photo_id = sqlite3_column_int64(st, 1);
        const void* blob = sqlite3_column_blob(st, 2);
        const int bytes = sqlite3_column_bytes(st, 2);
        if (blob != nullptr && bytes > 0 && bytes % (int)sizeof(float) == 0) {
            f.embedding.resize((size_t)bytes / sizeof(float));
            std::memcpy(f.embedding.data(), blob, (size_t)bytes);
        }
        out.push_back(std::move(f));
    }
    sqlite3_finalize(st);
    return true;
}

bool Db::replace_clusters(const std::vector<std::vector<long>>& groups,
                          std::string& err) {
    err.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    // Duplicates are input validation: no DB read needed. One face in two
    // groups would silently land in the last group holding it (review #51).
    std::set<long> grouped;
    for (const auto& g : groups)
        for (const long fid : g)
            if (!grouped.insert(fid).second) {
                err = "duplicate face id in groups";
                return false;
            }
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    // Unknown-id validation runs UNDER the write lock (CodeRabbit #51):
    // before the transaction a concurrent replace_faces could delete and
    // reinsert a face between this read and the assignment loop, making an
    // assignment UPDATE silently match zero rows.
    if (!grouped.empty()) {
        std::set<long> known;
        sqlite3_stmt* st = nullptr;
        bool valid = false;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM faces", -1, &st,
                               nullptr) != SQLITE_OK) {
            err = sqlite3_errmsg(db_);
        } else {
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                known.insert(sqlite3_column_int64(st, 0));
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) {
                err = sqlite3_errmsg(db_);
            } else {
                valid = true;
                for (const long fid : grouped)
                    if (known.count(fid) == 0) {
                        err = "unknown face id in groups";
                        valid = false;
                        break;
                    }
            }
        }
        if (!valid) {
            std::string rb;
            exec("ROLLBACK", rb);
            return false;
        }
    }
    bool ok = false;
    do {
        bool step_ok = true;
        // Current face -> live cluster map, for id/name preservation.
        std::map<long, long> face_cluster;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT id, cluster_id FROM faces WHERE cluster_id IS NOT NULL",
                    -1, &st, nullptr) != SQLITE_OK) {
                step_ok = false;
            } else {
                int rc;
                while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                    face_cluster[sqlite3_column_int64(st, 0)] =
                        sqlite3_column_int64(st, 1);
                if (rc != SQLITE_DONE) step_ok = false;
                sqlite3_finalize(st);
            }
        }
        if (!step_ok) break;
        // Global max-overlap assignment (review #51): score every
        // (group, live cluster) pair by shared faces and claim pairs
        // best-first, so the LARGEST fragment of a split cluster keeps
        // its id and name regardless of the caller's group order.
        struct Claim {
            long overlap;
            size_t gi;
            long cid;
        };
        std::vector<Claim> claims;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            if (groups[gi].empty()) continue;  // never insert 0-face rows
            std::map<long, long> overlap;
            for (const long fid : groups[gi]) {
                const auto it = face_cluster.find(fid);
                if (it != face_cluster.end()) ++overlap[it->second];
            }
            for (const auto& kv : overlap)
                claims.push_back({kv.second, gi, kv.first});
        }
        std::sort(claims.begin(), claims.end(),
                  [](const Claim& a, const Claim& b) {
                      if (a.overlap != b.overlap) return a.overlap > b.overlap;
                      if (a.gi != b.gi) return a.gi < b.gi;
                      return a.cid < b.cid;
                  });
        std::vector<long> group_cluster(groups.size(), 0);
        {
            std::set<long> used_clusters, used_groups;
            for (const Claim& c : claims) {
                if (used_groups.count(c.gi) != 0 ||
                    used_clusters.count(c.cid) != 0)
                    continue;
                group_cluster[c.gi] = c.cid;
                used_groups.insert(c.gi);
                used_clusters.insert(c.cid);
            }
        }
        std::set<long> reused;
        for (const long cid : group_cluster)
            if (cid != 0) reused.insert(cid);
        // Clear assignments: faces no group covers end at NULL rather
        // than pointing at a cluster they no longer belong to.
        if (sqlite3_exec(db_, "UPDATE faces SET cluster_id = NULL", nullptr,
                         nullptr, nullptr) != SQLITE_OK)
            break;
        // Drop live clusters no group kept.
        {
            std::vector<long> live_ids;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT id FROM clusters WHERE merged_into IS NULL",
                    -1, &st, nullptr) != SQLITE_OK) {
                step_ok = false;
            } else {
                int rc;
                while ((rc = sqlite3_step(st)) == SQLITE_ROW)
                    live_ids.push_back(sqlite3_column_int64(st, 0));
                if (rc != SQLITE_DONE) step_ok = false;
                sqlite3_finalize(st);
            }
            sqlite3_stmt* del = nullptr;
            if (step_ok && sqlite3_prepare_v2(db_,
                    "DELETE FROM clusters WHERE id = ?",
                    -1, &del, nullptr) != SQLITE_OK)
                step_ok = false;
            if (step_ok) {
                for (const long cid : live_ids) {
                    if (reused.count(cid) != 0) continue;
                    sqlite3_bind_int64(del, 1, cid);
                    if (sqlite3_step(del) != SQLITE_DONE) {
                        step_ok = false;
                        break;
                    }
                    sqlite3_reset(del);
                }
            }
            sqlite3_finalize(del);  // nullptr-safe
        }
        if (!step_ok) break;
        // Merged-away rows are audit history; a fresh recluster supersedes
        // them (their faces already moved to live clusters by the merge).
        if (sqlite3_exec(db_, "DELETE FROM clusters WHERE merged_into IS NOT NULL",
                         nullptr, nullptr, nullptr) != SQLITE_OK)
            break;
        for (size_t gi = 0; gi < groups.size() && step_ok; ++gi) {
            if (groups[gi].empty()) continue;  // skipped, never inserted
            if (group_cluster[gi] == 0) {
                sqlite3_stmt* ins = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "INSERT INTO clusters(name) VALUES(NULL)",
                        -1, &ins, nullptr) != SQLITE_OK) {
                    step_ok = false;
                    break;
                }
                if (sqlite3_step(ins) != SQLITE_DONE) {
                    sqlite3_finalize(ins);
                    step_ok = false;
                    break;
                }
                group_cluster[gi] = (long)sqlite3_last_insert_rowid(db_);
                sqlite3_finalize(ins);
            }
            sqlite3_stmt* upd = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "UPDATE faces SET cluster_id = ? WHERE id = ?",
                    -1, &upd, nullptr) != SQLITE_OK) {
                step_ok = false;
                break;
            }
            for (const long fid : groups[gi]) {
                sqlite3_bind_int64(upd, 1, group_cluster[gi]);
                sqlite3_bind_int64(upd, 2, fid);
                if (sqlite3_step(upd) != SQLITE_DONE) {
                    step_ok = false;
                    break;
                }
                sqlite3_reset(upd);
            }
            sqlite3_finalize(upd);
        }
        ok = step_ok;
    } while (false);

    if (ok) ok = exec("COMMIT", err);
    if (!ok) {
        // Capture SQLite's diagnostic BEFORE the rollback: ROLLBACK itself
        // can overwrite the connection's error state (CodeRabbit #45).
        if (err.empty()) err = sqlite3_errmsg(db_);
        std::string rb;
        exec("ROLLBACK", rb);  // best-effort; original error is the one to keep
    }
    return ok;
}

bool Db::people_list(std::vector<PersonSummary>& out, std::string& err) {
    out.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT c.id, COALESCE(c.name, ''),"
        " (SELECT count(*) FROM faces f WHERE f.cluster_id = c.id),"
        " COALESCE((SELECT f2.crop_path FROM faces f2"
        "          WHERE f2.cluster_id = c.id ORDER BY f2.id LIMIT 1), '')"
        " FROM clusters c WHERE c.merged_into IS NULL"
        " ORDER BY 3 DESC, c.id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    while (true) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            err = sqlite3_errmsg(db_);
            out.clear();
            sqlite3_finalize(st);
            return false;
        }
        PersonSummary ps;
        ps.id = sqlite3_column_int64(st, 0);
        ps.name = (const char*)sqlite3_column_text(st, 1);
        ps.face_count = sqlite3_column_int64(st, 2);
        ps.cover_crop = (const char*)sqlite3_column_text(st, 3);
        out.push_back(std::move(ps));
    }
    sqlite3_finalize(st);
    return true;
}

bool Db::person_faces(long cluster_id, std::vector<PersonFace>& out,
                      std::string& err) {
    out.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT f.id, f.bbox, f.crop_path, p.path"
        " FROM faces f JOIN photos p ON p.id = f.photo_id"
        " WHERE f.cluster_id = ? ORDER BY p.path, f.id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, cluster_id);
    while (true) {
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            err = sqlite3_errmsg(db_);
            out.clear();
            sqlite3_finalize(st);
            return false;
        }
        PersonFace pf;
        pf.id = sqlite3_column_int64(st, 0);
        pf.bbox = (const char*)sqlite3_column_text(st, 1);
        pf.crop_path = (const char*)sqlite3_column_text(st, 2);
        pf.photo_path = (const char*)sqlite3_column_text(st, 3);
        out.push_back(std::move(pf));
    }
    sqlite3_finalize(st);
    return true;
}

bool Db::rename_cluster(long cluster_id, const std::string& name,
                        std::string& err) {
    err.clear();
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE clusters SET name = NULLIF(?, '')"
            " WHERE id = ? AND merged_into IS NULL",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, cluster_id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    const bool found = ok && sqlite3_changes(db_) == 1;
    sqlite3_finalize(st);
    if (!ok) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    if (!found) {
        err = "no such live person";
        return false;
    }
    return true;
}

bool Db::merge_clusters(long from, long to, std::string& err) {
    err.clear();
    if (from == to) {
        err = "cannot merge a person into itself";
        return false;
    }
    if (!flush()) {
        err = "COMMIT of the pending scan batch failed";
        return false;
    }
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    bool ok = false;
    do {
        // Both ids must name LIVE clusters before anything moves.
        long live = 0;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT count(*) FROM clusters"
                    " WHERE id IN (?, ?) AND merged_into IS NULL",
                    -1, &st, nullptr) != SQLITE_OK)
                break;
            sqlite3_bind_int64(st, 1, from);
            sqlite3_bind_int64(st, 2, to);
            if (sqlite3_step(st) == SQLITE_ROW)
                live = sqlite3_column_int64(st, 0);
            else {
                sqlite3_finalize(st);
                break;
            }
            sqlite3_finalize(st);
        }
        if (live != 2) {
            err = "no such live person";
            break;
        }
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "UPDATE faces SET cluster_id = ? WHERE cluster_id = ?",
                    -1, &st, nullptr) != SQLITE_OK)
                break;
            sqlite3_bind_int64(st, 1, to);
            sqlite3_bind_int64(st, 2, from);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) break;
        }
        // An unnamed target adopts the source's name rather than losing
        // it with the source row (review #51); a named target keeps its
        // own name.
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "UPDATE clusters SET name = (SELECT name FROM clusters WHERE id = ?)"
                    " WHERE id = ? AND (name IS NULL OR name = '')",
                    -1, &st, nullptr) != SQLITE_OK) {
                ok = false;
                break;
            }
            sqlite3_bind_int64(st, 1, from);
            sqlite3_bind_int64(st, 2, to);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) break;
        }
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "UPDATE clusters SET merged_into = ? WHERE id = ?",
                -1, &st, nullptr) != SQLITE_OK) {
            ok = false;
            break;
        }
        sqlite3_bind_int64(st, 1, to);
        sqlite3_bind_int64(st, 2, from);
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
    } while (false);

    if (ok) ok = exec("COMMIT", err);
    if (!ok) {
        if (err.empty()) err = sqlite3_errmsg(db_);
        std::string rb;
        exec("ROLLBACK", rb);
    }
    return ok;
}

}  // namespace wfi
