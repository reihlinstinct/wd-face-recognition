#include "db.h"

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

}  // namespace wfi
