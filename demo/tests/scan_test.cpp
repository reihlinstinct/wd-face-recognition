// Unit tests for the recursive photo scan (issue #24). No ncnn needed:
// builds as g++ -std=c++17 scan_test.cpp ../src/scan.cpp -o scan_test.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>

#include "../src/scan.h"

namespace fs = std::filesystem;

// CHECK stays active in every build configuration: plain CHECK() compiles
// out under NDEBUG (the documented CMake build is Release), which would let
// ctest report a pass without checking anything.
static int g_failures = 0;
#define CHECK(cond)                                        \
    do {                                                   \
        if (!(cond)) {                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",      \
                         __FILE__, __LINE__, #cond);       \
            ++g_failures;                                  \
        }                                                  \
    } while (0)

namespace {

fs::path make_tree() {
    fs::path root = fs::temp_directory_path() /
                    ("wdf-scan-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "sub1" / "sub2");
    // Depth semantics: max_depth = number of nested folder levels below the
    // root whose files are collected. So d7 (8 levels down) is the last
    // folder entered with the default max_depth of 8.
    fs::path deep = root;
    for (int i = 0; i < 10; ++i) {  // chain deeper than any max_depth we test
        deep /= "d" + std::to_string(i);
        fs::create_directory(deep);
        if (i == 7) std::ofstream(deep / "mid.jpg").put('\0');  // exactly at the cap
    }
    std::ofstream(root / "a.jpg").put('\0');
    std::ofstream(root / "b.png").put('\0');
    std::ofstream(root / "noext").put('\0');
    std::ofstream(root / "sub1" / "c.JPEG").put('\0');
    std::ofstream(root / "sub1" / "sub2" / "d.jpg").put('\0');
    std::ofstream(deep / "deep.jpg").put('\0');  // 10 levels down: beyond the cap
    fs::create_symlink(root, root / "loop");                 // dir loop
    fs::create_symlink(root / "sub1", root / "linkdir");     // dir symlink
    fs::create_symlink(root / "a.jpg", root / "fake.jpg");   // file symlink
    return root;
}

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

int main() {
    // ends_jpeg units
    CHECK(wdf::ends_jpeg("a.jpg") && wdf::ends_jpeg("A.JPG"));
    CHECK(wdf::ends_jpeg("b.jpeg") && wdf::ends_jpeg("b.JPEG"));
    CHECK(!wdf::ends_jpeg("b.png") && !wdf::ends_jpeg("jpg"));
    CHECK(!wdf::ends_jpeg("a.jpgx") && !wdf::ends_jpeg(".jpg"));

    const fs::path root = make_tree();
    const std::string r = root.string();

    {   // recursion: nested JPEGs found, non-JPEG and symlinks excluded
        wdf::ScanResult res = wdf::scan_photos(r);
        CHECK(has(res.photos, "a.jpg"));
        CHECK(has(res.photos, "sub1/c.JPEG"));
        CHECK(has(res.photos, "sub1/sub2/d.jpg"));
        CHECK(!has(res.photos, "b.png"));
        CHECK(!has(res.photos, "noext"));
        CHECK(!has(res.photos, "fake.jpg"));          // symlinked file: not followed
        for (const auto& p : res.photos) {             // symlinked dirs: no duplicates via loop/linkdir
            CHECK(p.find("loop") == std::string::npos);
            CHECK(p.find("linkdir") == std::string::npos);
        }
        CHECK(has(res.photos, "d0/d1/d2/d3/d4/d5/d6/d7/mid.jpg"));   // exactly 8 levels down
        CHECK(!has(res.photos, "d0/d1/d2/d3/d4/d5/d6/d7/d8/d9/deep.jpg"));  // beyond cap
        CHECK(res.truncated_depth);
        CHECK(!res.truncated_photos);
        CHECK(std::is_sorted(res.photos.begin(), res.photos.end()));
    }
    {   // depth cap: files up to 2 nested folder levels are collected
        wdf::ScanOptions o;
        o.max_depth = 2;
        wdf::ScanResult res = wdf::scan_photos(r, o);
        CHECK(has(res.photos, "a.jpg"));
        CHECK(has(res.photos, "sub1/c.JPEG"));       // 1 level down
        CHECK(has(res.photos, "sub1/sub2/d.jpg"));   // 2 levels down: still inside
        CHECK(!has(res.photos, "d0/d1/d2/d3/d4/d5/d6/d7/mid.jpg"));  // 8 levels: beyond cap
        CHECK(res.truncated_depth);
    }
    {   // photo cap: more JPEGs exist past the cap -> truncated
        wdf::ScanOptions o;
        o.max_photos = 2;
        wdf::ScanResult res = wdf::scan_photos(r, o);
        CHECK(res.photos.size() == 2);
        CHECK(res.truncated_photos);
    }
    {   // exactly at the cap with only non-JPEGs after it -> NOT truncated
        fs::path exact = fs::temp_directory_path() /
                         ("wdf-scan-exact-" + std::to_string(::getpid()));
        fs::remove_all(exact);
        fs::create_directories(exact);
        std::ofstream(exact / "a.jpg").put('\0');
        std::ofstream(exact / "b.jpg").put('\0');
        std::ofstream(exact / "c.png").put('\0');
        wdf::ScanOptions o;
        o.max_photos = 2;
        wdf::ScanResult res = wdf::scan_photos(exact.string(), o);
        CHECK(res.photos.size() == 2);
        CHECK(!res.truncated_photos);
        fs::remove_all(exact);
    }
    {   // global entry budget: the walk stops even when entries are not photos
        wdf::ScanOptions o;
        o.max_entries = 3;
        wdf::ScanResult res = wdf::scan_photos(r, o);
        CHECK(res.truncated_entries);
        CHECK(res.entries_examined == 3);
        CHECK(res.photos.size() <= 3);
    }
    {   // flat dir, budget below the JPEG count: collected entries are kept
        fs::path flat = fs::temp_directory_path() /
                        ("wdf-scan-flat-" + std::to_string(::getpid()));
        fs::remove_all(flat);
        fs::create_directories(flat);
        for (int i = 0; i < 5; ++i)
            std::ofstream(flat / ("f" + std::to_string(i) + ".jpg")).put('\0');
        wdf::ScanOptions o;
        o.max_entries = 3;
        wdf::ScanResult res = wdf::scan_photos(flat.string(), o);
        CHECK(res.truncated_entries);
        CHECK(res.entries_examined == 3);
        CHECK(res.photos.size() == 3);
        fs::remove_all(flat);
    }
    {   // default budget: not exhausted by the small test tree
        wdf::ScanResult res = wdf::scan_photos(r);
        CHECK(!res.truncated_entries);
        CHECK(res.entries_examined > 0);
    }
    {   // missing root: empty, not a crash
        wdf::ScanResult res = wdf::scan_photos(r + "/does-not-exist");
        CHECK(res.photos.empty());
    }
    {   // A symlink as the scan root itself: opendir follows it, so the
        // content IS scanned - the safety property is that symlinks INSIDE
        // the tree are never followed (asserted above via loop/linkdir).
        // The web UI separately pins the root under /shares/.
        wdf::ScanResult res = wdf::scan_photos((root / "loop").string());
        CHECK(!res.photos.empty());
    }

    fs::remove_all(root);
    if (g_failures > 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("scan_test: all assertions passed");
    return 0;
}
