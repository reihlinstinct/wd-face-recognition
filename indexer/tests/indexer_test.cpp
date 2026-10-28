// Indexer skeleton tests (issue #40): full mtime-delta lifecycle against a
// real SQLite DB in /tmp - first scan queues everything, second scan skips,
// mtime change re-queues exactly one, deletion marks missing, reappearance
// re-queues. Run: ./wfi_test
#include <atomic>
#include <cstdio>
#include <fcntl.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utime.h>
#include <vector>

#include "../src/config.h"
#include "../src/cropreserve.h"
#include "../src/db.h"
#include "../src/walk.h"

static int fails = 0;
static void check(bool cond, const char* label) {
    if (!cond) {
        ++fails;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

static void write_file(const std::string& path, const std::string& bytes) {
    FILE* f = std::fopen(path.c_str(), "w");
    std::fputs(bytes.c_str(), f);
    std::fclose(f);
}

int main() {
    std::string tmp = std::string("/tmp/wfi-test-") + std::to_string(getpid());
    std::string root = tmp + "/photos";
    std::string data = tmp + "/data";
    mkdir(tmp.c_str(), 0777);
    mkdir(root.c_str(), 0777);
    mkdir((root + "/sub").c_str(), 0777);
    mkdir(data.c_str(), 0777);
    write_file(root + "/a.jpg", "aaaa");
    write_file(root + "/sub/b.jpg", "bbbb");
    write_file(root + "/notes.txt", "not a photo");

    std::string err;
    wfi::Db db;
    check(db.open(data + "/index.db", err), "db opens");

    // Config parsing.
    write_file(tmp + "/wfi.conf", "# comment\nroots=" + root + "\ndata_dir=" + data + "\n");
    wfi::Config cfg;
    check(wfi::load_config(tmp + "/wfi.conf", cfg, err), "config loads");
    check(cfg.roots.size() == 1 && cfg.roots[0] == root, "config root parsed");
    wfi::Config bad;
    check(!wfi::load_config(tmp + "/missing.conf", bad, err), "missing config refused");

    // First scan: two JPEGs queued (txt ignored).
    long job1 = db.begin_job(root);
    wfi::ScanCounts c1;
    check(wfi::scan_root(db, job1, root, 8, c1, err), "first scan runs");
    db.sweep_missing(job1, root);
    db.finish_job(job1, c1);
    check(c1.scanned == 2 && c1.added == 2 && c1.skipped == 0 && c1.changed == 0,
          "first scan queues all JPEGs");

    // Second scan, unchanged: everything skipped.
    long job2 = db.begin_job(root);
    wfi::ScanCounts c2;
    check(wfi::scan_root(db, job2, root, 8, c2, err), "second scan runs");
    db.sweep_missing(job2, root);
    check(c2.scanned == 2 && c2.skipped == 2 && c2.added == 0 && c2.changed == 0,
          "unchanged second scan is stat-only (all skipped)");

    // Touch one file's mtime: exactly one re-queued.
    struct stat st;
    stat((root + "/a.jpg").c_str(), &st);
    struct utimbuf tb { st.st_atime, st.st_mtime + 100 };
    utime((root + "/a.jpg").c_str(), &tb);
    long job3 = db.begin_job(root);
    wfi::ScanCounts c3;
    check(wfi::scan_root(db, job3, root, 8, c3, err), "third scan runs");
    db.sweep_missing(job3, root);
    check(c3.changed == 1 && c3.skipped == 1, "mtime change re-queues exactly one");

    // Delete one: marked missing. Restore: re-queued.
    unlink((root + "/sub/b.jpg").c_str());
    long job4 = db.begin_job(root);
    wfi::ScanCounts c4;
    check(wfi::scan_root(db, job4, root, 8, c4, err), "fourth scan runs");
    long miss4 = db.sweep_missing(job4, root);
    check(miss4 == 1, "deleted photo marked missing");
    write_file(root + "/sub/b.jpg", "bbbb");
    long job5 = db.begin_job(root);
    wfi::ScanCounts c5;
    check(wfi::scan_root(db, job5, root, 8, c5, err), "fifth scan runs");
    db.sweep_missing(job5, root);
    check(c5.changed == 1 || c5.added == 1, "reappeared photo re-queued");

    // Depth cap: a photo deeper than max-depth is not seen.
    mkdir((root + "/d1").c_str(), 0777);
    std::string deep = root + "/d1";
    for (int i = 2; i <= 9; ++i) {
        std::string next = deep + "/d" + std::to_string(i);
        mkdir(next.c_str(), 0777);
        deep = next;
    }
    write_file(deep + "/deep.jpg", "deep");
    long job6 = db.begin_job(root);
    wfi::ScanCounts c6;
    check(wfi::scan_root(db, job6, root, 8, c6, err), "sixth scan runs");
    db.sweep_missing(job6, root);
    check(c6.added == 0 && c6.changed == 0, "photo beyond depth cap not indexed");

    // Unreadable dir => incomplete scan; sweep must be skipped by the
    // caller, so no false "missing" marks (CodeRabbit #41). As a
    // non-root user chmod 000 blocks opendir; as root it does not, so
    // only assert the flag when the dir was actually unreadable.
    chmod((root + "/sub").c_str(), 0000);
    long job7 = db.begin_job(root);
    wfi::ScanCounts c7;
    check(wfi::scan_root(db, job7, root, 8, c7, err), "seventh scan runs");
    if (c7.incomplete) {
        check(c7.scanned < 2, "unreadable dir reduced the scan");
        // main would skip the sweep here; verify no rows were harmed.
    }
    chmod((root + "/sub").c_str(), 0777);

    // Depth-capped sweep protection (CodeRabbit #41 round 2): index the
    // deep photo with a large cap, then rescan with a small cap - the
    // walk flags depth_capped and the caller skips the sweep, so the
    // deep row must never be falsely marked missing.
    long job8 = db.begin_job(root);
    wfi::ScanCounts c8;
    check(wfi::scan_root(db, job8, root, 16, c8, err), "eighth scan (deep)");
    check(c8.added == 1, "deep photo indexed with large cap");
    db.sweep_missing(job8, root);
    long job9 = db.begin_job(root);
    wfi::ScanCounts c9;
    check(wfi::scan_root(db, job9, root, 2, c9, err), "ninth scan (capped)");
    check(c9.depth_capped, "small cap flags depth_capped");
    // main skips the sweep on depth_capped (emulated: no sweep call). A
    // following full scan must see the deep photo as unchanged, not as
    // back-from-missing (which would count as changed).
    long job10 = db.begin_job(root);
    wfi::ScanCounts c10;
    check(wfi::scan_root(db, job10, root, 16, c10, err), "tenth scan (deep)");
    db.sweep_missing(job10, root);
    check(c10.added == 0 && c10.changed == 0,
          "deep photo survived the capped scan untouched");

    // Face-pipeline DB contract (issue #40 PR 2): queued_photos lists the
    // queue, replace_faces swaps rows atomically and reports old crops,
    // mark_failed records the reason. No ncnn involved at this layer.
    // The scans above legitimately leave three photos queued: a.jpg (job3
    // touch), sub/b.jpg (job5 reappearance), the deep photo (job8 add).
    {
        std::vector<wfi::QueuedPhoto> q;
        check(db.queued_photos(100, q, err), "queue query succeeds");
        check(q.size() == 3, "three photos queued from the scans above");
        long aid = 0, bid = 0;
        int64_t asz = 0, amt = 0, bsz = 0, bmt = 0;
        for (const auto& p : q) {
            if (p.path == root + "/a.jpg") { aid = p.id; asz = p.size; amt = p.mtime; }
            if (p.path == root + "/sub/b.jpg") { bid = p.id; bsz = p.size; bmt = p.mtime; }
        }
        check(aid > 0 && bid > 0, "queued ids resolved");

        wfi::FaceRow r1;
        r1.bbox = "1,2,3,4";
        r1.embedding = {0.5f, -0.25f, 1.0f};
        r1.crop_path = "crops/p" + std::to_string(aid) + "_f0.jpg";
        std::vector<std::string> oldc;
        bool conflict = true;  // must come back false on a clean commit
        check(db.replace_faces(aid, asz, amt, {r1}, oldc, conflict, err) && !conflict,
              "first replace_faces commits");
        check(oldc.empty(), "no old crops on first process");
        check(db.queued_photos(100, q, err) && q.size() == 2, "processed photo leaves the queue");

        // Second round (reprocess): the old crop path comes back so the
        // caller can unlink the stale file.
        wfi::FaceRow r2;
        r2.bbox = "5,6,7,8";
        r2.embedding = {0.1f};
        r2.crop_path = "crops/p" + std::to_string(aid) + "_f0.jpg";
        oldc.clear();
        // The guard only commits from 'queued' state: after the first
        // replace the photo is 'scanned', so an immediate second replace
        // conflicts (production re-processing always re-queues via a scan).
        conflict = false;
        check(!db.replace_faces(aid, asz, amt, {r2}, oldc, conflict, err) && conflict,
              "scanned photo refuses a blind re-replace");
        check(oldc.empty(), "refused commit reports no crops to unlink");
        // Re-queue the photo (changed mtime), as the walk would.
        long jobq = db.begin_job(root);
        check(db.observe(jobq, root, root + "/a.jpg", asz, amt + 1, err) == 2,
              "changed photo re-queued");
        // A stale version (the pre-change mtime) must NOT commit either.
        conflict = false;
        err = "database is locked";  // stale error from a previous call
        check(!db.replace_faces(aid, asz, amt, {r2}, oldc, conflict, err) && conflict,
              "stale version refuses to commit");
        check(err.empty(),
              "conflict does not inherit a stale lock error (CodeRabbit #45)");
        conflict = true;
        check(db.replace_faces(aid, asz, amt + 1, {r2}, oldc, conflict, err) && !conflict,
              "second replace_faces commits");
        check(oldc.size() == 1 && oldc[0] == r1.crop_path, "old crop path reported");

        // A failed photo carries its reason and leaves the queue.
        conflict = false;
        check(!db.mark_failed(bid, bsz, bmt + 1, "decode", conflict, err) && conflict,
              "stale mark_failed refuses");
        conflict = true;
        check(db.mark_failed(bid, bsz, bmt, "decode", conflict, err) && !conflict,
              "mark_failed commits");
        check(db.queued_photos(100, q, err) && q.size() == 1, "failed photo leaves the queue");
        check(db.queued_count() == 1, "queued_count reports the real queue depth");
        // The refused replaces above must have rolled back completely:
        // the first round's face row survives (review #45).
        check(db.face_count(aid) == 1, "refused replace keeps committed face rows");

        // Busy/rollback (review #45): a second connection holding the write
        // lock makes replace_faces fail with "locked" after the (shortened)
        // busy timeout - and the committed rows stay untouched.
        {
            // Re-queue a.jpg BEFORE the lock is taken (begin_job/observe
            // need the write lock themselves), and commit the batch.
            long jobl = db.begin_job(root);
            check(db.observe(jobl, root, root + "/a.jpg", asz, amt + 2, err) == 2,
                  "re-queue before busy test");
            check(db.flush(), "re-queue committed");
            sqlite3* raw = nullptr;  // grab an exclusive write lock directly
            check(sqlite3_open((data + "/index.db").c_str(), &raw) == SQLITE_OK,
                  "raw lock connection opens");
            sqlite3_busy_timeout(raw, 5000);
            check(sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK,
                  "write lock taken");
            db.set_busy_timeout(150);  // keep the test fast
            check(db.replace_faces(aid, asz, amt + 2, {r2}, oldc, conflict, err) == false
                      && !conflict && err.find("locked") != std::string::npos,
                  "busy writer makes replace_faces fail with locked");
            check(db.face_count(aid) == 1, "busy failure keeps committed rows");
            check(sqlite3_exec(raw, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK,
                  "write lock released");
            sqlite3_close(raw);
            db.set_busy_timeout(5000);
            conflict = true;
            check(db.replace_faces(aid, asz, amt + 2, {r2}, oldc, conflict, err) && !conflict,
                  "replace succeeds after the lock clears");
        }
    }

    // Crop-path reservation (CodeRabbit #45): two concurrent process runs
    // starting in the same second derive the same stem, so the path must be
    // claimed exclusively. A loser must get its own path and must never be
    // able to truncate or unlink the winner's crop.
    {
        const std::string cdir = data + "/crops";
        check(mkdir(cdir.c_str(), 0700) == 0, "crop dir created");
        const std::string stem = "p7_t123_f0";

        std::string p1, p2;
        check(wfi::reserve_crop_file(cdir, stem, p1, err), "first reservation succeeds");
        check(wfi::reserve_crop_file(cdir, stem, p2, err), "second reservation succeeds despite collision");
        check(p1 == cdir + "/" + stem + ".jpg", "first reservation gets the base path");
        check(p2 == cdir + "/" + stem + "_r1.jpg", "loser gets a suffixed path");
        check(p1 != p2, "racing writers get distinct paths");

        struct stat st1, st2;
        check(stat(p1.c_str(), &st1) == 0 && stat(p2.c_str(), &st2) == 0,
              "both reserved files exist");
        check(st1.st_size == 0 && st2.st_size == 0, "reserved files start empty");
        check((st1.st_mode & 0777) == 0600 && (st2.st_mode & 0777) == 0600,
              "reserved files are owner-only 0600");

        // The winner can rewrite its own reservation (the encoder's "wb"
        // open only ever truncates bytes this run owns).
        write_file(p1, "winner-bytes");
        write_file(p2, "loser-bytes");
        char buf[16] = {0};
        FILE* rf = std::fopen(p1.c_str(), "rb");
        check(rf != nullptr && std::fread(buf, 1, 12, rf) == 12 &&
                  std::string(buf) == "winner-bytes",
              "winner's bytes survive the loser's reservation");
        if (rf) std::fclose(rf);

        // Exhaustion: all 100 candidates taken -> clean refusal.
        for (int i = 2; i < 100; ++i)
            write_file(cdir + "/" + stem + "_r" + std::to_string(i) + ".jpg", "");
        std::string px;
        check(!wfi::reserve_crop_file(cdir, stem, px, err),
              "reservation refuses when every candidate exists");

        // A real race: two threads grabbing paths for one stem from the
        // same starting gate must never be handed the same path.
        const std::string rstem = "p7_t123_f1";
        std::atomic<bool> go{false};
        std::mutex mu;
        std::set<std::string> claimed;
        auto racer = [&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 8; ++i) {
                std::string rp, rerr;
                if (!wfi::reserve_crop_file(cdir, rstem, rp, rerr)) continue;
                std::lock_guard<std::mutex> lk(mu);
                claimed.insert(rp);
            }
        };
        std::thread t1(racer), t2(racer);
        go.store(true, std::memory_order_release);
        t1.join();
        t2.join();
        check(claimed.size() == 16, "16 racing reservations all land on distinct paths");
    }

    // ensure_private_dir (independent review #45): a pre-existing wider
    // directory is tightened to 0700 rather than trusted, a missing one is
    // created 0700, and a non-directory is refused.
    {
        const std::string wdir = data + "/wide";
        check(mkdir(wdir.c_str(), 0755) == 0, "wide dir created");
        struct stat stw0;
        check(stat(wdir.c_str(), &stw0) == 0 && (stw0.st_mode & 0777) != 0700,
              "wide dir starts wider than 0700");
        check(wfi::ensure_private_dir(wdir, err), "ensure_private_dir accepts an existing dir");
        struct stat stw;
        check(stat(wdir.c_str(), &stw) == 0 && (stw.st_mode & 0777) == 0700,
              "existing dir tightened to 0700");
        const std::string ndir = data + "/fresh-dir";
        check(wfi::ensure_private_dir(ndir, err), "ensure_private_dir creates a missing dir");
        struct stat stn;
        check(stat(ndir.c_str(), &stn) == 0 && (stn.st_mode & 0777) == 0700,
              "fresh dir created 0700");
        const std::string notdir = data + "/not-a-dir";
        write_file(notdir, "x");
        check(!wfi::ensure_private_dir(notdir, err), "non-directory refused");
    }

    // A pre-existing group/world-readable index.db must be tightened to
    // 0600 on open (independent review #45). umask masks FRESH files, so
    // only a pre-created 0644 file catches a missing chmod.
    {
        const std::string d2 = tmp + "/data-wide";
        check(mkdir(d2.c_str(), 0700) == 0, "second data dir created");
        const std::string dbp = d2 + "/index.db";
        int fd = open(dbp.c_str(), O_WRONLY | O_CREAT, 0644);
        check(fd >= 0, "0644 db file pre-created");
        if (fd >= 0) close(fd);
        check(chmod(dbp.c_str(), 0644) == 0, "0644 mode forced past umask");
        wfi::Db db2;
        check(db2.open(dbp, err), "db opens over the pre-created file");
        struct stat stb;
        check(stat(dbp.c_str(), &stb) == 0 && (stb.st_mode & 0777) == 0600,
              "pre-existing 0644 index.db tightened to 0600");
    }

    // People methods (issue #50, PR 3a): clustering rewrite with global
    // max-overlap id/name preservation, people_list/person_faces, rename,
    // merge. One face row (a.jpg's) is already committed from the
    // pipeline tests above.
    long first_id = 0;  // Ima's cluster id, for any later block
    {
        // Queue two fresh photos and seed one face each.
        write_file(root + "/p1.jpg", "p1-bytes");
        write_file(root + "/p2.jpg", "p2-bytes");
        long jobp = db.begin_job(root);
        check(db.observe(jobp, root, root + "/p1.jpg", 8, 1000, err) == 1, "p1 queued");
        check(db.observe(jobp, root, root + "/p2.jpg", 8, 1000, err) == 1, "p2 queued");
        check(db.flush(), "seed batch committed");

        std::vector<wfi::QueuedPhoto> pq;
        check(db.queued_photos(100, pq, err), "seed queue readable");
        long p1id = 0, p2id = 0;
        int64_t p1sz = 0, p1mt = 0, p2sz = 0, p2mt = 0;
        for (const auto& qq : pq) {
            if (qq.path == root + "/p1.jpg") { p1id = qq.id; p1sz = qq.size; p1mt = qq.mtime; }
            if (qq.path == root + "/p2.jpg") { p2id = qq.id; p2sz = qq.size; p2mt = qq.mtime; }
        }
        check(p1id > 0 && p2id > 0, "seed ids resolved");
        auto seed_face = [&](long pid, int64_t sz, int64_t mt, float embv,
                             const std::string& crop) {
            wfi::FaceRow r;
            r.bbox = "0,0,1,1";
            r.embedding = {embv, embv + 1.0f};
            r.crop_path = crop;
            std::vector<std::string> oldc;
            bool conflict = true;
            check(db.replace_faces(pid, sz, mt, {r}, oldc, conflict, err) && !conflict,
                  "seed face committed");
        };
        seed_face(p1id, p1sz, p1mt, 0.1f, "crops/p1_t1_f0.jpg");
        seed_face(p2id, p2sz, p2mt, 0.2f, "crops/p2_t1_f0.jpg");

        // all_face_embeddings: oldest first (ORDER BY mutant), exact
        // float values (round-trip mutant), independent of leftover rows.
        std::vector<wfi::ClusterFace> cf;
        check(db.all_face_embeddings(cf, err), "all_face_embeddings reads");
        check(cf.size() == 3, "all faces read (a.jpg leftover + two seeds)");
        check(cf[0].id < cf[1].id && cf[1].id < cf[2].id, "faces ordered by id");
        const long aface = cf[0].id, p1face = cf[1].id, p2face = cf[2].id;
        check(cf[0].embedding.size() == 1 && cf[0].embedding[0] == 0.1f,
              "leftover embedding round-trips exactly");
        check(cf[1].embedding.size() == 2 &&
                  cf[1].embedding[0] == 0.1f && cf[1].embedding[1] == 1.1f,
              "seeded embedding values round-trip exactly");

        // Validation refusals write nothing (review #51).
        check(!db.replace_clusters({{p1face, p1face}}, err) &&
                  err.find("duplicate") != std::string::npos,
              "duplicate face id refused");
        check(!db.replace_clusters({{999999}}, err) &&
                  err.find("unknown") != std::string::npos,
              "unknown face id refused");
        std::vector<wfi::PersonSummary> pl;
        check(db.people_list(pl, err) && pl.empty(),
              "refusals leave no clusters behind");

        // Three one-face groups: cover = the only face's crop.
        check(db.replace_clusters({{aface}, {p1face}, {p2face}}, err),
              "replace_clusters commits");
        check(db.people_list(pl, err) && pl.size() == 3, "three people listed");
        for (const auto& ps : pl) {
            check(ps.face_count == 1, "one face per person");
            check(!ps.cover_crop.empty(), "cover crop populated");
        }
        // person_faces contents + unknown id.
        long c_a = 0, c_p1 = 0, c_p2 = 0;
        for (const auto& ps : pl) {
            if (ps.cover_crop == "crops/p1_t1_f0.jpg") c_p1 = ps.id;
            else if (ps.cover_crop == "crops/p2_t1_f0.jpg") c_p2 = ps.id;
            else c_a = ps.id;
        }
        check(c_a > 0 && c_p1 > 0 && c_p2 > 0, "cluster ids resolved");
        std::vector<wfi::PersonFace> pf;
        check(db.person_faces(c_p1, pf, err) && pf.size() == 1 &&
                  pf[0].photo_path == root + "/p1.jpg" &&
                  pf[0].crop_path == "crops/p1_t1_f0.jpg",
              "person face carries photo path and crop");
        check(db.person_faces(999999, pf, err) && pf.empty(),
              "unknown person is empty, not an error");

        // Rename + clear (NULLIF mutant) + unknown refusal.
        check(db.rename_cluster(c_p1, "Ima", err), "rename commits");
        check(db.rename_cluster(c_p2, "Temp", err), "second rename commits");
        check(db.rename_cluster(c_p2, "", err), "empty name clears");
        check(db.people_list(pl, err), "people_list after renames");
        for (const auto& ps : pl) {
            if (ps.id == c_p1) check(ps.name == "Ima", "name visible");
            if (ps.id == c_p2) check(ps.name.empty(), "cleared name is empty");
        }
        // Raw SQL discrimination (mutant: NULLIF dropped -> '' stored):
        // a cleared name must be NULL in clusters, not an empty string.
        {
            sqlite3* raw = nullptr;
            check(sqlite3_open((data + "/index.db").c_str(), &raw) == SQLITE_OK,
                  "raw name connection opens");
            sqlite3_stmt* st = nullptr;
            check(sqlite3_prepare_v2(raw,
                      "SELECT name IS NULL FROM clusters WHERE id = ?",
                      -1, &st, nullptr) == SQLITE_OK,
                  "name-null query prepares");
            sqlite3_bind_int64(st, 1, c_p2);
            check(sqlite3_step(st) == SQLITE_ROW &&
                      sqlite3_column_int64(st, 0) == 1,
                  "cleared name is NULL, not an empty string");
            sqlite3_finalize(st);
            sqlite3_close(raw);
        }
        check(!db.rename_cluster(999999, "x", err) && !err.empty(),
              "rename of unknown person refused with a reason");

        // Recluster of the same groups keeps id AND name on each cluster.
        check(db.replace_clusters({{aface}, {p1face}, {p2face}}, err),
              "second recluster commits");
        check(db.people_list(pl, err), "people_list after recluster");
        for (const auto& ps : pl) {
            if (ps.cover_crop == "crops/p1_t1_f0.jpg")
                check(ps.id == c_p1 && ps.name == "Ima",
                      "recluster preserves id and name");
        }

        // Merge into the NAMED cluster: target keeps its name; cover of a
        // multi-face cluster is the lowest face id's crop (cover mutant);
        // person_faces is ordered by photo path (ordering mutant).
        check(!db.merge_clusters(c_p1, c_p1, err), "self-merge refused");
        check(!db.merge_clusters(c_p1, 999999, err), "merge into unknown refused");
        check(db.merge_clusters(c_p2, c_p1, err), "merge commits");
        check(db.people_list(pl, err) && pl.size() == 2, "merged source leaves the list");
        for (const auto& ps : pl)
            if (ps.id == c_p1) {
                check(ps.face_count == 2 && ps.name == "Ima",
                      "named target keeps its name");
                check(ps.cover_crop == "crops/p1_t1_f0.jpg",
                      "cover is the lowest face id's crop");
            }
        check(db.person_faces(c_p1, pf, err) && pf.size() == 2 &&
                  pf[0].photo_path < pf[1].photo_path,
              "person faces ordered by photo path");
        check(!db.rename_cluster(c_p2, "ghost", err),
              "rename of a merged-away cluster refused");

        // Name adoption: merging INTO an unnamed target moves the name
        // with the faces instead of losing it (review #51).
        check(db.merge_clusters(c_p1, c_a, err), "merge into unnamed commits");
        check(db.people_list(pl, err) && pl.size() == 1 &&
                  pl[0].id == c_a && pl[0].name == "Ima" && pl[0].face_count == 3,
              "unnamed target adopts the source name");

        // Clear step (mutant): from the single 3-face cluster, recluster
        // only 2 of its faces; the survivor keeps the id and the dropped
        // face must NOT linger on it.
        check(db.replace_clusters({{p1face, p2face}}, err),
              "two-face recluster commits");
        check(db.person_faces(c_a, pf, err) && pf.size() == 2,
              "uncovered face left the surviving cluster");
        check(db.replace_clusters({{aface, p1face, p2face}}, err),
              "3-face state restored");

        // Split rule (review #51): a singleton group listed FIRST must not
        // steal the id+name from the larger fragment.
        check(db.replace_clusters({{aface}, {p1face, p2face}}, err),
              "split recluster commits");
        check(db.people_list(pl, err) && pl.size() == 2, "split leaves two people");
        check(pl[0].face_count == 2 && pl[1].face_count == 1,
              "people listed by face count descending");
        for (const auto& ps : pl) {
            if (ps.face_count == 2)
                check(ps.id == c_a && ps.name == "Ima",
                      "largest fragment keeps id and name");
        }
        check(db.person_faces(c_a, pf, err) && pf.size() == 2,
              "largest fragment holds two faces");

        // Clear step (mutant): a face no group covers ends at NULL rather
        // than pointing at its old cluster. aface drops out here.
        check(db.replace_clusters({{p1face, p2face}}, err),
              "partial recluster commits");
        check(db.person_faces(c_a, pf, err) && pf.size() == 2,
              "uncovered face left the old cluster");
        for (const auto& f : pf)
            check(f.crop_path.find("p1_") != std::string::npos ||
                      f.crop_path.find("p2_") != std::string::npos,
                  "only covered faces remain");

        // Empty groups are skipped, never inserted as 0-face rows.
        check(db.replace_clusters({{}, {p1face, p2face, aface}}, err),
              "empty-group recluster commits");
        check(db.people_list(pl, err) && pl.size() == 1 && pl[0].face_count == 3,
              "empty group adds no 0-face person");

        // Merge-audit rows are deleted by a recluster (mutant): none may
        // remain after the merges above.
        {
            sqlite3* raw = nullptr;
            check(sqlite3_open((data + "/index.db").c_str(), &raw) == SQLITE_OK,
                  "raw audit connection opens");
            sqlite3_stmt* st = nullptr;
            check(sqlite3_prepare_v2(raw,
                      "SELECT count(*) FROM clusters WHERE merged_into IS NOT NULL",
                      -1, &st, nullptr) == SQLITE_OK,
                  "audit query prepares");
            check(sqlite3_step(st) == SQLITE_ROW &&
                      sqlite3_column_int64(st, 0) == 0,
                  "no merged audit rows survive a recluster");
            sqlite3_finalize(st);
            // faces(cluster_id) must be indexed: people_list, person_faces
            // and merge_clusters all filter on it (CodeRabbit #51).
            check(sqlite3_prepare_v2(raw,
                      "SELECT count(*) FROM sqlite_master"
                      " WHERE type='index' AND name='idx_faces_cluster'",
                      -1, &st, nullptr) == SQLITE_OK,
                  "index query prepares");
            check(sqlite3_step(st) == SQLITE_ROW &&
                      sqlite3_column_int64(st, 0) == 1,
                  "faces(cluster_id) is indexed");
            sqlite3_finalize(st);
            sqlite3_close(raw);
        }

        // person_faces ordering (mutant: ORDER BY f.id instead of
        // p.path, f.id): queue z2.jpg BEFORE a2.jpg so the z-face gets the
        // lower face id but the higher path; photo-path order must win.
        write_file(root + "/z2.jpg", "z2-bytes");
        write_file(root + "/a2.jpg", "a2-bytes");
        long jobo = db.begin_job(root);
        check(db.observe(jobo, root, root + "/z2.jpg", 8, 1000, err) == 1, "z2 queued");
        check(db.observe(jobo, root, root + "/a2.jpg", 8, 1000, err) == 1, "a2 queued");
        check(db.flush(), "ordering batch committed");
        long z2id = 0, a2id = 0;
        int64_t z2sz = 0, z2mt = 0, a2sz = 0, a2mt = 0;
        check(db.queued_photos(100, pq, err), "ordering queue readable");
        for (const auto& qq : pq) {
            if (qq.path == root + "/z2.jpg") { z2id = qq.id; z2sz = qq.size; z2mt = qq.mtime; }
            if (qq.path == root + "/a2.jpg") { a2id = qq.id; a2sz = qq.size; a2mt = qq.mtime; }
        }
        check(z2id > 0 && a2id > 0 && z2id < a2id, "z2 queued before a2");
        seed_face(z2id, z2sz, z2mt, 0.3f, "crops/z2_t1_f0.jpg");
        seed_face(a2id, a2sz, a2mt, 0.4f, "crops/a2_t1_f0.jpg");
        long zface = 0, a2face = 0;
        check(db.all_face_embeddings(cf, err), "faces re-read");
        for (const auto& f : cf) {
            if (f.photo_id == z2id) zface = f.id;
            if (f.photo_id == a2id) a2face = f.id;
        }
        check(zface > 0 && a2face > 0 && zface < a2face,
              "z-face id below a-face id");
        check(db.replace_clusters({{aface, p1face, p2face}, {zface, a2face}}, err),
              "ordering recluster commits");
        check(db.people_list(pl, err), "people_list after ordering recluster");
        long c_pair = 0;
        for (const auto& ps : pl)
            if (ps.cover_crop == "crops/z2_t1_f0.jpg") c_pair = ps.id;
        check(c_pair > 0, "pair cluster resolved");
        check(db.person_faces(c_pair, pf, err) && pf.size() == 2 &&
                  pf[0].photo_path == root + "/a2.jpg" &&
                  pf[1].photo_path == root + "/z2.jpg",
              "person faces ordered by photo path, not face id");

        // Ima's cluster id for any later block.
        check(db.people_list(pl, err) && pl.size() == 2 && pl[0].name == "Ima",
              "final state: one named person");
        first_id = pl[0].id;
    }

    if (fails > 0) {
        std::fprintf(stderr, "%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("indexer_test: all assertions passed\n");
    return 0;
}
