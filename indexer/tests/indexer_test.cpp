// Indexer skeleton tests (issue #40): full mtime-delta lifecycle against a
// real SQLite DB in /tmp - first scan queues everything, second scan skips,
// mtime change re-queues exactly one, deletion marks missing, reappearance
// re-queues. Run: ./wfi_test
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "../src/config.h"
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

    if (fails > 0) {
        std::fprintf(stderr, "%d test(s) failed\n", fails);
        return 1;
    }
    std::printf("indexer_test: all assertions passed\n");
    return 0;
}
