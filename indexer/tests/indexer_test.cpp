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
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utime.h>
#include <vector>

#include "../src/config.h"
#include "../src/cropreserve.h"
#include "../src/lockfile.h"
#include "../src/db.h"
#include "../src/people.h"
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
    long first_id = 0;  // Ima's cluster id, reused by the recluster tests
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
        seed_face(z2id, z2sz, z2mt, -0.9f, "crops/z2_t1_f0.jpg");
        seed_face(a2id, a2sz, a2mt, -0.95f, "crops/a2_t1_f0.jpg");
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

    // --threshold parsing (issue #67): validated strtod, finite, [-1,1].
    {
        double th = 99.0;
        check(wfi::parse_threshold("0.5", th) && th == 0.5, "plain value parses");
        check(wfi::parse_threshold("-1", th) && th == -1.0, "lower boundary ok");
        check(wfi::parse_threshold("1", th) && th == 1.0, "upper boundary ok");
        check(wfi::parse_threshold("1e-3", th) && th == 0.001, "exponent ok");
        check(!wfi::parse_threshold("abc", th), "malformed refused");
        check(!wfi::parse_threshold("0.5x", th), "trailing junk refused");
        check(!wfi::parse_threshold("", th), "empty refused");
        check(!wfi::parse_threshold("nan", th), "NaN refused");
        check(!wfi::parse_threshold("inf", th), "inf refused");
        check(!wfi::parse_threshold("1e-400", th),
              "underflow (ERANGE) refused, not silently 0");
        check(!wfi::parse_threshold("1.5", th), "above range refused");
        check(!wfi::parse_threshold("-2", th), "below range refused");
    }

    // CLI option parsing (issue #68): unknown flags and a trailing option
    // without a value must be rejected, never silently ignored.
    {
        wfi::CliOptions opt;
        std::string oerr;
        char a0[] = "wdf_faces", a1[] = "cluster", a2[] = "--config",
             a3[] = "c.conf", a4[] = "--threshold", a5[] = "0.7";
        char* ok_argv[] = {a0, a1, a2, a3, a4, a5};
        check(wfi::parse_cli_options(6, ok_argv, 2, "cluster", opt, oerr) &&
                  opt.config_path == "c.conf" && opt.threshold == 0.7,
              "valid options parse");
        char b2[] = "--bogus", b3[] = "1";
        char* bad_argv[] = {a0, a1, b2, b3};
        check(!wfi::parse_cli_options(4, bad_argv, 2, "cluster", opt, oerr) &&
                  oerr.find("unknown") != std::string::npos,
              "unknown option refused");
        char* trail_argv[] = {a0, a1, a4};
        check(!wfi::parse_cli_options(3, trail_argv, 2, "cluster", opt, oerr) &&
                  oerr.find("needs a value") != std::string::npos,
              "trailing valueless option refused");
        char t5[] = "abc";
        char* badth_argv[] = {a0, a1, a4, t5};
        check(!wfi::parse_cli_options(4, badth_argv, 2, "cluster", opt, oerr),
              "bad threshold refused at parse");
    }

    // Per-subcommand options (issue #69): cluster refuses process flags
    // and vice versa, before the index opens.
    {
        wfi::CliOptions opt;
        std::string oerr;
        char a0[] = "wdf_faces", a1[] = "cluster", a2[] = "--limit",
             a3[] = "10";
        char* cross_argv[] = {a0, a1, a2, a3};
        check(!wfi::parse_cli_options(4, cross_argv, 2, "cluster", opt, oerr) &&
                  oerr.find("not valid for cluster") != std::string::npos,
              "cluster refuses --limit");
        char t2[] = "--threshold", t3[] = "0.7";
        char* cth_argv[] = {a0, a1, t2, t3};
        check(!wfi::parse_cli_options(4, cth_argv, 2, "process", opt, oerr) &&
                  oerr.find("not valid for process") != std::string::npos,
              "process refuses --threshold");
        char m2[] = "--models", m3[] = "/m";
        char* cm_argv[] = {a0, a1, m2, m3};
        check(!wfi::parse_cli_options(4, cm_argv, 2, "cluster", opt, oerr),
              "cluster refuses --models");
        char th2[] = "--threads", th3[] = "4";
        char* cth2_argv[] = {a0, a1, th2, th3};
        check(!wfi::parse_cli_options(4, cth2_argv, 2, "cluster", opt, oerr),
              "cluster refuses --threads");
        char c2[] = "--config", c3[] = "c.conf", l2[] = "--limit", l3[] = "10";
        char* pok_argv[] = {a0, a1, c2, c3, l2, l3};
        check(wfi::parse_cli_options(6, pok_argv, 2, "process", opt, oerr) &&
                  opt.limit == 10,
              "process accepts its own flags");
    }

    // Integer option validation (issue #69): full consume + range.
    {
        wfi::CliOptions opt;
        std::string oerr;
        char a0[] = "wdf_faces", a1[] = "process", f1[] = "--limit";
        char v1[] = "10x";
        char* junk_argv[] = {a0, a1, f1, v1};
        check(!wfi::parse_cli_options(4, junk_argv, 2, "process", opt, oerr),
              "limit with trailing junk refused");
        char v2[] = "0";
        char* zero_argv[] = {a0, a1, f1, v2};
        check(!wfi::parse_cli_options(4, zero_argv, 2, "process", opt, oerr),
              "limit 0 refused");
        char v3[] = "-5";
        char* neg_argv[] = {a0, a1, f1, v3};
        check(!wfi::parse_cli_options(4, neg_argv, 2, "process", opt, oerr),
              "negative limit refused");
        char v4[] = "100001";
        char* big_argv[] = {a0, a1, f1, v4};
        check(!wfi::parse_cli_options(4, big_argv, 2, "process", opt, oerr),
              "limit above cap refused");
        // Overflow-length input is refused END-TO-END (issue #70): strtol
        // clamps and the range check rejects it; this does not pin the
        // errno branch in parse_long_range (review #74).
        char v9[] = "99999999999999999999999";
        char* ovf_argv[] = {a0, a1, f1, v9};
        check(!wfi::parse_cli_options(4, ovf_argv, 2, "process", opt, oerr),
              "overflow-length limit refused (issue #70)");
        char i2[] = "--id";
        char* ovi_argv[] = {a0, a1, i2, v9};
        check(!wfi::parse_cli_options(4, ovi_argv, 2, "person", opt, oerr),
              "overflow-length id refused (issue #70)");
        // Negative underflow mirrors the positive case (issue #82):
        // strtol clamps to LONG_MIN and the range check rejects it
        // end-to-end.
        char vn[] = "-99999999999999999999999";
        char* udf_argv[] = {a0, a1, f1, vn};
        check(!wfi::parse_cli_options(4, udf_argv, 2, "process", opt, oerr),
              "negative-overflow limit refused (issue #82)");
        char* udi_argv[] = {a0, a1, i2, vn};
        check(!wfi::parse_cli_options(4, udi_argv, 2, "person", opt, oerr),
              "negative-overflow id refused (issue #82)");
        char v5[] = "1";
        char* edge_argv[] = {a0, a1, f1, v5};
        check(wfi::parse_cli_options(4, edge_argv, 2, "process", opt, oerr) &&
                  opt.limit == 1,
              "limit 1 is the valid edge");
        char f2[] = "--threads", t1[] = "0";
        char* tz_argv[] = {a0, a1, f2, t1};
        check(!wfi::parse_cli_options(4, tz_argv, 2, "process", opt, oerr),
              "threads 0 refused");
        char t2[] = "33";
        char* tb_argv[] = {a0, a1, f2, t2};
        check(!wfi::parse_cli_options(4, tb_argv, 2, "process", opt, oerr),
              "threads above cap refused");
        char t3[] = "1";
        char* te_argv[] = {a0, a1, f2, t3};
        check(wfi::parse_cli_options(4, te_argv, 2, "process", opt, oerr) &&
                  opt.threads_override == 1,
              "threads 1 is the valid edge");
    }

    // Per-subcommand options for the people commands (#50 PR 3c, extending
    // issue #69): each accepts only its own flags; ids are validated.
    {
        wfi::CliOptions opt;
        std::string oerr;
        char a0[] = "wdf_faces", c2[] = "--config", c3[] = "c.conf";
        char p1[] = "person", i2[] = "--id", i3[] = "7";
        char* person_argv[] = {a0, p1, c2, c3, i2, i3};
        check(wfi::parse_cli_options(6, person_argv, 2, "person", opt, oerr) &&
                  opt.id_arg == 7,
              "person accepts --config --id");
        char l2[] = "--limit", l3[] = "10";
        char* pl_argv[] = {a0, p1, c2, c3, l2, l3};
        check(!wfi::parse_cli_options(6, pl_argv, 2, "person", opt, oerr) &&
                  oerr.find("not valid for person") != std::string::npos,
              "person refuses --limit");
        char m1[] = "merge", f2[] = "--from", f3[] = "3",
             t2[] = "--to", t3[] = "4";
        char* merge_argv[] = {a0, m1, c2, c3, f2, f3, t2, t3};
        check(wfi::parse_cli_options(8, merge_argv, 2, "merge", opt, oerr) &&
                  opt.from_arg == 3 && opt.to_arg == 4,
              "merge accepts --from --to");
        char r1[] = "rename", n2[] = "--name", n3[] = "Dana";
        char* rn_argv[] = {a0, r1, c2, c3, i2, i3, n2, n3};
        check(wfi::parse_cli_options(8, rn_argv, 2, "rename", opt, oerr) &&
                  opt.id_arg == 7 && opt.name_arg == "Dana",
              "rename accepts --id --name");
        char pe1[] = "people";
        char* people_argv[] = {a0, pe1, c2, c3};
        check(wfi::parse_cli_options(4, people_argv, 2, "people", opt, oerr),
              "people accepts --config only");
        char* pi_argv[] = {a0, pe1, c2, c3, i2, i3};
        check(!wfi::parse_cli_options(6, pi_argv, 2, "people", opt, oerr) &&
                  oerr.find("not valid for people") != std::string::npos,
              "people refuses --id");
        char z3[] = "0";
        char* pz_argv[] = {a0, p1, c2, c3, i2, z3};
        check(!wfi::parse_cli_options(6, pz_argv, 2, "person", opt, oerr),
              "id 0 refused");
        char j3[] = "7x";
        char* pj_argv[] = {a0, p1, c2, c3, i2, j3};
        check(!wfi::parse_cli_options(6, pj_argv, 2, "person", opt, oerr),
              "id with trailing junk refused");
    }

    // recluster (issue #50, PR 3b): union-find over stored embeddings at
    // cosine > 0.5. Four controlled faces: two near-identical pairs plus
    // an orthogonal one; the five faces from the people tests above ride
    // along (p1/p2 and z2/a2 each merge into their own pair, the 1-dim
    // one stays alone via the size guard).
    {
        write_file(root + "/q1.jpg", "q1-bytes");
        write_file(root + "/q2.jpg", "q2-bytes");
        write_file(root + "/q3.jpg", "q3-bytes");
        write_file(root + "/q4.jpg", "q4-bytes");
        long jobr = db.begin_job(root);
        for (const char* n : {"q1", "q2", "q3", "q4"})
            check(db.observe(jobr, root, root + "/" + n + ".jpg", 8, 1000, err) == 1,
                  "recluster photo queued");
        check(db.flush(), "recluster seed batch committed");
        std::vector<wfi::QueuedPhoto> rq;
        check(db.queued_photos(200, rq, err), "recluster queue readable");
        // 3-dim seeds: orthogonal to the 2-dim people-test embeddings, so
        // only the intended pairs unite (the size guard skips cross pairs).
        auto seed2 = [&](const char* name, float e0, float e1, float e2) {
            for (const auto& qq : rq) {
                if (qq.path != root + "/" + name + ".jpg") continue;
                wfi::FaceRow r;
                r.bbox = "0,0,1,1";
                r.embedding = {e0, e1, e2};
                r.crop_path = std::string("crops/") + name + "_t1_f0.jpg";
                std::vector<std::string> oldc;
                bool conflict = true;
                check(db.replace_faces(qq.id, qq.size, qq.mtime, {r}, oldc,
                                       conflict, err) && !conflict,
                      "recluster face committed");
            }
        };
        seed2("q1", 1.0f, 0.0f, 0.0f);
        seed2("q2", 0.98f, 0.02f, 0.0f);  // ~identical direction to q1
        seed2("q3", 0.0f, 1.0f, 0.0f);
        seed2("q4", 0.0f, 0.98f, 0.02f);  // ~identical direction to q3

        wfi::ClusterCounts cc;
        check(wfi::recluster(db, 0.5, cc, err), "recluster commits");
        check(cc.faces == 9, "all faces clustered");
        // q1+q2, q3+q4, p1+p2 (near-parallel 2-dim), and the 1-dim
        // singleton (size guard keeps it out of every pair).
        check(cc.people == 5, "expected group count");
        check(cc.singletons == 1, "only the dimension-skewed face is alone");

        // Cluster membership via cover crops (cover = first face's crop).
        std::vector<wfi::PersonSummary> rp;
        check(db.people_list(rp, err), "people_list after recluster");
        auto cluster_of = [&](const char* crop) -> long {
            for (const auto& ps : rp) {
                std::vector<wfi::PersonFace> rf;
                check(db.person_faces(ps.id, rf, err), "person faces readable");
                for (const auto& f : rf)
                    if (f.crop_path == crop) return ps.id;
            }
            return 0;
        };
        const long c_q1 = cluster_of("crops/q1_t1_f0.jpg");
        check(c_q1 > 0 && c_q1 == cluster_of("crops/q2_t1_f0.jpg"),
              "near-identical pair shares a person");
        check(cluster_of("crops/q3_t1_f0.jpg") == cluster_of("crops/q4_t1_f0.jpg"),
              "second pair shares a person");
        check(cluster_of("crops/q3_t1_f0.jpg") != c_q1,
              "orthogonal pairs stay apart");
        // Name preservation through recluster: the p1/p2 pair carries
        // Ima's id from the people tests above.
        check(cluster_of("crops/p1_t1_f0.jpg") == first_id,
              "recluster keeps the named cluster on its largest fragment");
        check(db.people_list(rp, err) && rp.size() == 5, "five people listed");
        bool ima_named = false;
        for (const auto& ps : rp) if (ps.id == first_id && ps.name == "Ima") ima_named = true;
        check(ima_named, "name survives the full recluster");
    }

    // recluster group ordering tiebreak (issue #64): equal-size groups
    // must come out smallest-face-id-first, NOT in union-find root order.
    // Pair A spans the lowest and highest face ids ({f1,f4}), pair B the
    // two middle ones ({f2,f3}); unite() roots each pair at its HIGHER
    // index, so root order is [B, A] while the contract order is [A, B].
    // replace_clusters inserts new cluster ids in group order and
    // people_list breaks size ties by id, so pl[0] reveals the order.
    // Isolated Db so earlier blocks' clusters cannot pollute the list.
    {
        std::string tmp2 = tmp + "/tie";
        std::string root2 = tmp2 + "/photos";
        std::string data2 = tmp2 + "/data";
        mkdir(tmp2.c_str(), 0777);
        mkdir(root2.c_str(), 0777);
        mkdir(data2.c_str(), 0777);
        wfi::Db db2;
        check(db2.open(data2 + "/index.db", err), "tiebreak db opens");
        for (const char* n : {"t1", "t2", "t3", "t4"})
            write_file(root2 + "/" + n + ".jpg", n);
        long jobt = db2.begin_job(root2);
        for (const char* n : {"t1", "t2", "t3", "t4"})
            check(db2.observe(jobt, root2, root2 + "/" + n + ".jpg", 8, 1000, err) == 1,
                  "tiebreak photo queued");
        check(db2.flush(), "tiebreak batch committed");
        std::vector<wfi::QueuedPhoto> tq;
        check(db2.queued_photos(100, tq, err), "tiebreak queue readable");
        // Seed in path order t1..t4 so face ids ascend t1<t2<t3<t4.
        auto seed3 = [&](const char* name, float e0, float e1, float e2) {
            for (const auto& qq : tq) {
                if (qq.path != root2 + "/" + name + ".jpg") continue;
                wfi::FaceRow r;
                r.bbox = "0,0,1,1";
                r.embedding = {e0, e1, e2};
                r.crop_path = std::string("crops/") + name + "_t1_f0.jpg";
                std::vector<std::string> oldc;
                bool conflict = true;
                check(db2.replace_faces(qq.id, qq.size, qq.mtime, {r}, oldc,
                                        conflict, err) && !conflict,
                      "tiebreak face committed");
            }
        };
        seed3("t1", 1.0f, 0.0f, 0.0f);
        seed3("t2", 0.0f, 1.0f, 0.0f);
        seed3("t3", 0.0f, 0.98f, 0.02f);   // ~t2
        seed3("t4", 0.98f, 0.0f, 0.02f);   // ~t1
        wfi::ClusterCounts tc;
        check(wfi::recluster(db2, 0.5, tc, err), "tiebreak recluster commits");
        check(tc.people == 2 && tc.singletons == 0, "two equal pairs");
        std::vector<wfi::PersonSummary> tpl;
        check(db2.people_list(tpl, err), "tiebreak people listed");
        check(tpl.size() == 2 && tpl[0].face_count == 2 &&
                  tpl[1].face_count == 2,
              "two 2-face people");
        check(tpl[0].cover_crop == "crops/t1_t1_f0.jpg" &&
                  tpl[1].cover_crop == "crops/t2_t1_f0.jpg",
              "equal-size groups ordered by smallest face id, not root order");
        check(tpl[0].id < tpl[1].id, "group order fixes insert order");
    }
    // replace_clusters competing-overlap claim rule (issue #71): C
    // splits 3/2 across G1/G2 while D wholly joins G1 with 4. Global
    // max-overlap awards G1 to D (4 > 3); C's id/name continue on G2.
    {
        std::string tmp3 = tmp + "/claim";
        std::string root3 = tmp3 + "/photos";
        std::string data3 = tmp3 + "/data";
        mkdir(tmp3.c_str(), 0777);
        mkdir(root3.c_str(), 0777);
        mkdir(data3.c_str(), 0777);
        wfi::Db db3;
        check(db3.open(data3 + "/index.db", err), "claim db opens");
        const char* names[9] = {"c1", "c2", "c3", "c4", "c5",
                                "d1", "d2", "d3", "d4"};
        for (const char* n : names)
            write_file(root3 + "/" + n + ".jpg", n);
        long jobc = db3.begin_job(root3);
        for (const char* n : names)
            check(db3.observe(jobc, root3, root3 + "/" + n + ".jpg", 8, 1000, err) == 1,
                  "claim photo queued");
        check(db3.flush(), "claim batch committed");
        std::vector<wfi::QueuedPhoto> cq;
        check(db3.queued_photos(100, cq, err), "claim queue readable");
        for (const auto& qq : cq) {  // one face per photo, ascending ids
            wfi::FaceRow r;
            r.bbox = "0,0,1,1";
            r.embedding = {0.5f, 0.5f};
            r.crop_path = "crops/" + qq.path.substr(root3.size() + 1);
            std::vector<std::string> oldc;
            bool conflict = true;
            check(db3.replace_faces(qq.id, qq.size, qq.mtime, {r}, oldc,
                                    conflict, err) && !conflict,
                  "claim face committed");
        }
        std::vector<wfi::ClusterFace> cf3;
        check(db3.all_face_embeddings(cf3, err) && cf3.size() == 9,
              "claim faces read in id order");
        const long f[9] = {cf3[0].id, cf3[1].id, cf3[2].id, cf3[3].id,
                           cf3[4].id, cf3[5].id, cf3[6].id, cf3[7].id,
                           cf3[8].id};
        check(db3.replace_clusters({{f[0], f[1], f[2], f[3], f[4]},
                                    {f[5], f[6], f[7], f[8]}}, err),
              "claim initial clusters commit");
        std::vector<wfi::PersonSummary> pl3;
        check(db3.people_list(pl3, err) && pl3.size() == 2, "two clusters");
        long c_id = 0, d_id = 0;
        for (const auto& ps : pl3) {
            if (ps.cover_crop == "crops/c1.jpg") c_id = ps.id;
            if (ps.cover_crop == "crops/d1.jpg") d_id = ps.id;
        }
        check(c_id > 0 && d_id > 0, "claim cluster ids resolved");
        check(db3.rename_cluster(c_id, "Split", err) &&
                  db3.rename_cluster(d_id, "Absorb", err),
              "claim clusters named");

        // C splits 3/2; D's 4 faces all land in G1.
        check(db3.replace_clusters({{f[0], f[1], f[2], f[5], f[6], f[7], f[8]},
                                    {f[3], f[4]}}, err),
              "claim recluster commits");
        check(db3.people_list(pl3, err) && pl3.size() == 2, "still two people");
        bool absorbed_ok = false, split_ok = false;
        for (const auto& ps : pl3) {
            if (ps.id == d_id)
                absorbed_ok = ps.name == "Absorb" && ps.face_count == 7;
            if (ps.id == c_id)
                split_ok = ps.name == "Split" && ps.face_count == 2;
        }
        check(absorbed_ok, "absorbing cluster keeps the contested group");
        check(split_ok, "split cluster's id/name continue on its fragment");
    }

    // replace_clusters claim tie (issue #71): equal overlaps for one
    // group are decided by the lowest cluster id, deterministically.
    {
        std::string tmp4 = tmp + "/tie2";
        std::string root4 = tmp4 + "/photos";
        std::string data4 = tmp4 + "/data";
        mkdir(tmp4.c_str(), 0777);
        mkdir(root4.c_str(), 0777);
        mkdir(data4.c_str(), 0777);
        wfi::Db db4;
        check(db4.open(data4 + "/index.db", err), "tie db opens");
        const char* names4[4] = {"e1", "e2", "f1", "f2"};
        for (const char* n : names4)
            write_file(root4 + "/" + n + ".jpg", n);
        long jobe = db4.begin_job(root4);
        for (const char* n : names4)
            check(db4.observe(jobe, root4, root4 + "/" + n + ".jpg", 8, 1000, err) == 1,
                  "tie photo queued");
        check(db4.flush(), "tie batch committed");
        std::vector<wfi::QueuedPhoto> eq;
        check(db4.queued_photos(100, eq, err), "tie queue readable");
        for (const auto& qq : eq) {
            wfi::FaceRow r;
            r.bbox = "0,0,1,1";
            r.embedding = {0.5f, 0.5f};
            r.crop_path = "crops/" + qq.path.substr(root4.size() + 1);
            std::vector<std::string> oldc;
            bool conflict = true;
            check(db4.replace_faces(qq.id, qq.size, qq.mtime, {r}, oldc,
                                    conflict, err) && !conflict,
                  "tie face committed");
        }
        std::vector<wfi::ClusterFace> cf4;
        check(db4.all_face_embeddings(cf4, err) && cf4.size() == 4,
              "tie faces read");
        const long g[4] = {cf4[0].id, cf4[1].id, cf4[2].id, cf4[3].id};
        check(db4.replace_clusters({{g[0], g[1]}, {g[2], g[3]}}, err),
              "tie initial clusters commit");
        std::vector<wfi::PersonSummary> pl4;
        check(db4.people_list(pl4, err) && pl4.size() == 2, "tie: two clusters");
        long e_id = 0, f_id = 0;
        for (const auto& ps : pl4) {
            if (ps.cover_crop == "crops/e1.jpg") e_id = ps.id;
            if (ps.cover_crop == "crops/f1.jpg") f_id = ps.id;
        }
        check(e_id > 0 && f_id > 0 && e_id < f_id, "E inserted before F");
        check(db4.rename_cluster(e_id, "Low", err) &&
                  db4.rename_cluster(f_id, "High", err),
              "tie clusters named");
        check(db4.replace_clusters({{g[0], g[1], g[2], g[3]}}, err),
              "tie recluster commits");
        check(db4.people_list(pl4, err), "tie people listed");
        check(pl4.size() == 1 && pl4[0].id == e_id &&
                  pl4[0].name == "Low" && pl4[0].face_count == 4,
              "overlap tie goes to the lowest cluster id");
    }

    // replace_clusters cross-group tie (CodeRabbit #75): one live
    // cluster overlapping TWO new groups equally is claimed by the
    // EARLIER group (caller group order decides before cluster id).
    {
        std::string tmp5 = tmp + "/xtie";
        std::string root5 = tmp5 + "/photos";
        std::string data5 = tmp5 + "/data";
        mkdir(tmp5.c_str(), 0777);
        mkdir(root5.c_str(), 0777);
        mkdir(data5.c_str(), 0777);
        wfi::Db db5;
        check(db5.open(data5 + "/index.db", err), "xtie db opens");
        const char* names5[5] = {"k1", "k2", "k3", "k4", "x1"};
        for (const char* n : names5)
            write_file(root5 + "/" + n + ".jpg", n);
        long jobk = db5.begin_job(root5);
        for (const char* n : names5)
            check(db5.observe(jobk, root5, root5 + "/" + n + ".jpg", 8, 1000, err) == 1,
                  "xtie photo queued");
        check(db5.flush(), "xtie batch committed");
        std::vector<wfi::QueuedPhoto> kq;
        check(db5.queued_photos(100, kq, err), "xtie queue readable");
        for (const auto& qq : kq) {
            wfi::FaceRow r;
            r.bbox = "0,0,1,1";
            r.embedding = {0.5f, 0.5f};
            r.crop_path = "crops/" + qq.path.substr(root5.size() + 1);
            std::vector<std::string> oldc;
            bool conflict = true;
            check(db5.replace_faces(qq.id, qq.size, qq.mtime, {r}, oldc,
                                    conflict, err) && !conflict,
                  "xtie face committed");
        }
        std::vector<wfi::ClusterFace> cf5;
        check(db5.all_face_embeddings(cf5, err) && cf5.size() == 5,
              "xtie faces read");
        const long h[5] = {cf5[0].id, cf5[1].id, cf5[2].id, cf5[3].id,
                           cf5[4].id};
        check(db5.replace_clusters({{h[0], h[1], h[2], h[3]}, {h[4]}}, err),
              "xtie initial clusters commit");
        std::vector<wfi::PersonSummary> pl5;
        check(db5.people_list(pl5, err) && pl5.size() == 2, "xtie: two clusters");
        long k_id = 0;
        for (const auto& ps : pl5)
            if (ps.cover_crop == "crops/k1.jpg") k_id = ps.id;
        check(k_id > 0 && db5.rename_cluster(k_id, "Even", err),
              "xtie cluster named");

        // C splits 2/2 across G1 (earlier) and G2: G1 keeps C's id/name.
        check(db5.replace_clusters({{h[0], h[1], h[4]}, {h[2], h[3]}}, err),
              "xtie recluster commits");
        check(db5.people_list(pl5, err) && pl5.size() == 2,
              "xtie: still two people");
        bool earlier_kept = false, later_new = false;
        for (const auto& ps : pl5) {
            if (ps.cover_crop == "crops/k1.jpg")
                earlier_kept = ps.id == k_id && ps.name == "Even" &&
                               ps.face_count == 3;
            if (ps.cover_crop == "crops/k3.jpg")
                later_new = ps.id != k_id && ps.name.empty() &&
                            ps.face_count == 2;
        }
        check(earlier_kept, "cross-group tie: earlier group keeps id and name");
        check(later_new, "cross-group tie: later group gets a fresh cluster");
    }

    // PRAGMA foreign_keys (issue #48): deleting a photo cascades its
    // faces. Without the per-connection pragma the face row would
    // survive with a dangling photo_id and face_count would stay 1.
    {
        std::string tmp5 = tmp + "/fk";
        std::string root5 = tmp5 + "/photos";
        std::string data5 = tmp5 + "/data";
        mkdir(tmp5.c_str(), 0777);
        mkdir(root5.c_str(), 0777);
        mkdir(data5.c_str(), 0777);
        wfi::Db db5;
        check(db5.open(data5 + "/index.db", err), "fk db opens");
        write_file(root5 + "/k1.jpg", "k1");
        long jobk = db5.begin_job(root5);
        check(db5.observe(jobk, root5, root5 + "/k1.jpg", 2, 1000, err) == 1,
              "fk photo queued");
        check(db5.flush(), "fk batch committed");
        std::vector<wfi::QueuedPhoto> kq;
        check(db5.queued_photos(10, kq, err) && kq.size() == 1,
              "fk queue readable");
        wfi::FaceRow fr;
        fr.bbox = "0,0,1,1";
        fr.embedding = {1.0f, 0.0f, 0.0f};
        fr.crop_path = "crops/k1_t1_f0.jpg";
        std::vector<std::string> oldc;
        bool conflict = true;
        check(db5.replace_faces(kq[0].id, kq[0].size, kq[0].mtime, {fr},
                                oldc, conflict, err) && !conflict,
              "fk face committed");
        check(db5.face_count(kq[0].id) == 1, "fk face counted");
        std::vector<std::string> dfc;
        check(db5.delete_photo(kq[0].id, dfc, err), "photo deleted");
        check(dfc.size() == 1 && dfc[0] == "crops/k1_t1_f0.jpg",
              "delete returns the crop paths for the caller to unlink");
        check(db5.face_count(kq[0].id) == 0,
              "faces cascade-deleted with the photo");
        // CodeRabbit #94: replacing faces on a deleted photo must come
        // back as a conflict (a normal GC race), not an FK abort.
        std::vector<std::string> oldc2;
        bool conflict2 = false;
        check(!db5.replace_faces(kq[0].id, kq[0].size, kq[0].mtime, {fr},
                                 oldc2, conflict2, err) && conflict2 &&
                  oldc2.empty(),
              "replace on a deleted photo is a conflict, not an FK error");
        // CodeRabbit #94: cascade deletes and per-photo lookups use the
        // idempotent idx_faces_photo index, created for existing DBs too.
        sqlite3* raw5 = nullptr;
        check(sqlite3_open((data5 + "/index.db").c_str(), &raw5) == SQLITE_OK,
              "raw open for the index check");
        sqlite3_stmt* ist = nullptr;
        check(sqlite3_prepare_v2(raw5,
                  "SELECT 1 FROM sqlite_master WHERE type='index'"
                  " AND name='idx_faces_photo'",
                  -1, &ist, nullptr) == SQLITE_OK,
              "index query prepares");
        check(sqlite3_step(ist) == SQLITE_ROW, "idx_faces_photo exists");
        sqlite3_finalize(ist);
        sqlite3_close(raw5);
    }

    // Instance lock (issue #46): a second holder is refused politely, the
    // lockfile leaves with its holder, and a stale lockfile (dead PID) is
    // reclaimed with the new holder's PID written in.
    {
        const std::string lockp = tmp + "/wdf.lock";
        {
            wfi::InstanceLock l1;
            check(l1.acquire(lockp, err), "first instance takes the lock");
            wfi::InstanceLock l2;
            check(!l2.acquire(lockp, err) &&
                      err.find("already running") != std::string::npos,
                  "second instance refused politely");
        }
        check(access(lockp.c_str(), F_OK) != 0,
              "lockfile released with the holder");
        // fork+wait gives a PID that is certainly dead afterwards.
        const pid_t dead = fork();
        if (dead == 0) _exit(0);
        check(dead > 0 && waitpid(dead, nullptr, 0) == dead,
              "dead pid available for the stale-lock test");
        const std::string stale = std::to_string((long)dead) + "\n";
        const int lfd = open(lockp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        check(lfd >= 0 && write(lfd, stale.c_str(), stale.size()) ==
                                 (ssize_t)stale.size(),
              "stale lockfile planted");
        if (lfd >= 0) close(lfd);
        wfi::InstanceLock l3;
        check(l3.acquire(lockp, err), "stale lockfile reclaimed");
        char buf[32] = {0};
        const int rfd = open(lockp.c_str(), O_RDONLY);
        check(rfd >= 0 && read(rfd, buf, sizeof(buf) - 1) > 0 &&
                  std::strtol(buf, nullptr, 10) == (long)getpid(),
              "reclaimed lock holds the new owner's pid");
        if (rfd >= 0) close(rfd);
        // CodeRabbit #97: the holder PID lands complete (temp+link), and
        // the temp file never survives.
        const std::string want = std::to_string((long)getpid()) + "\n";
        check(buf == want, "lockfile content is exactly the holder pid");
        check(access((lockp + ".tmp." + std::to_string((long)getpid())).c_str(),
                     F_OK) != 0,
              "no temp lockfile left behind");
        // A numeric prefix with trailing junk is NOT a live holder: with
        // only a strtol prefix parse this file would name our own live
        // pid and block every later instance.
        {
            const std::string junk = want.substr(0, want.size() - 1) + "junk\n";
            const int jfd = open(lockp.c_str(), O_WRONLY | O_TRUNC);
            check(jfd >= 0 && write(jfd, junk.c_str(), junk.size()) ==
                                     (ssize_t)junk.size(),
                  "junk-suffix lockfile planted");
            if (jfd >= 0) close(jfd);
            wfi::InstanceLock l4;
            check(l4.acquire(lockp, err),
                  "junk suffix is not a live pid (reclaimed)");
        }
        // An empty lockfile (a legacy or crashed writer) is stale too.
        {
            wfi::InstanceLock l5;
            check(l5.acquire(lockp, err), "lock re-acquired after release");
            const int efd = open(lockp.c_str(), O_WRONLY | O_TRUNC);
            if (efd >= 0) close(efd);  // l5 still "holds" it; dropped below
            wfi::InstanceLock l6;
            check(l6.acquire(lockp, err), "empty lockfile reclaimed");
        }
    }

    // JSON for the PHP pages (issue #50, PR 3c): escaping and shape.
    {
        check(wfi::json_escape("plain") == "plain", "plain text untouched");
        check(wfi::json_escape("a\"b\\c") == "a\\\"b\\\\c",
              "quotes and backslashes escaped");
        check(wfi::json_escape("l1\nl2\t!") == "l1\\nl2\\t!",
              "control chars escaped");
        // Hebrew passes through as UTF-8 bytes (\xD7\x90 = aleph).
        check(wfi::json_escape("\xD7\x90\xD7\x9E\xD7\x90") ==
                  "\xD7\x90\xD7\x9E\xD7\x90",
              "Hebrew UTF-8 passes through");

        std::vector<wfi::PersonSummary> people;
        check(db.people_list(people, err), "people list for json");
        const std::string pj = wfi::people_json(people);
        check(pj.front() == '[' && pj.back() == ']', "people json is an array");
        check(pj.find("\"id\":") != std::string::npos &&
                  pj.find("\"name\":\"Ima\"") != std::string::npos &&
                  pj.find("\"faces\":") != std::string::npos &&
                  pj.find("\"cover\":\"crops/") != std::string::npos,
              "people json carries id/name/faces/cover");

        wfi::PersonSummary who;
        for (const auto& ps : people) if (ps.name == "Ima") who = ps;
        std::vector<wfi::PersonFace> pfaces;
        check(db.person_faces(who.id, pfaces, err), "person faces for json");
        const std::string sj = wfi::person_json(who, pfaces);
        check(sj.find("\"name\":\"Ima\"") != std::string::npos &&
                  sj.find("\"photo\":\"") != std::string::npos &&
                  sj.find("\"bbox\":\"0,0,1,1\"") != std::string::npos,
              "person json carries name, photos, bboxes");

        // A hostile name must not break the JSON shape.
        wfi::PersonSummary nasty;
        nasty.id = 7;
        nasty.name = "x\",\"name\":\"evil";
        const std::string nj = wfi::person_json(nasty, {});
        check(nj.find("x\\\",\\\"name\\\":\\\"evil") != std::string::npos,
              "hostile name stays inside its string");
    }

    if (fails > 0) {
        std::fprintf(stderr, "%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("indexer_test: all assertions passed\n");
    return 0;
}
