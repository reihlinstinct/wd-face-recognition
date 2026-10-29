#include "walk.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <vector>

#include "db.h"

namespace wfi {

namespace {

bool ends_jpeg(const std::string& n) {
    std::string lower = n;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    return (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".jpg") == 0) ||
           (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0);
}

bool walk_dir(Db& db, long job_id, const std::string& root, const std::string& abs,
              int depth, int max_depth, ScanCounts& c, std::string& err) {
    DIR* d = opendir(abs.c_str());
    if (!d) {
        // Unreadable dir (unmounted share, transient perms): the scan is
        // INCOMPLETE - sweep_missing must not run for this root, or every
        // row under it would be marked missing by mistake (CodeRabbit #41).
        c.incomplete = true;
        return true;
    }
    std::vector<std::string> names;
    errno = 0;  // readdir returns NULL at end-of-dir AND on error
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    const int rd_err = errno;
    closedir(d);
    if (rd_err != 0) {
        // Mid-listing failure (share I/O error): the listing is partial,
        // so coverage is incomplete - no missing sweep (CodeRabbit #41).
        c.incomplete = true;
    }
    // Sorted traversal: deterministic job logs and DB write order.
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        const std::string child = abs + "/" + name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) {
            // ENOENT is benign (entry vanished between readdir and lstat);
            // anything else (I/O error, perms) means partial coverage -
            // flag incomplete so the missing sweep is skipped (CodeRabbit).
            if (errno != ENOENT) c.incomplete = true;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {  // lstat: symlinked dirs never followed
            if (depth < max_depth) {
                if (!walk_dir(db, job_id, root, child, depth + 1, max_depth, c,
                              err)) {
                    return false;
                }
            } else {
                // Not descending: deeper indexed paths are unobserved, so the
                // missing sweep would mark them gone by mistake (CodeRabbit
                // #41). Flag it; main skips the sweep like an unreadable dir.
                c.depth_capped = true;
            }
        } else if (S_ISREG(st.st_mode) && ends_jpeg(name)) {
            // (stale lstat between listing and stat is fine here: worst case
            // one photo is observed with an old mtime and re-queued once.)
            ++c.scanned;
            int r = db.observe(job_id, root, child, (int64_t)st.st_size,
                               (int64_t)st.st_mtime, err);
            if (r < 0) return false;
            if (r == 1) ++c.added;
            else if (r == 2) ++c.changed;
            else if (r == 3) ++c.restored;
            else ++c.skipped;
        }
    }
    return true;
}

}  // namespace

bool scan_root(Db& db, long job_id, const std::string& root,
               int max_depth, ScanCounts& c, std::string& err) {
    return walk_dir(db, job_id, root, root, 0, max_depth, c, err);
}

namespace {

// Split into segments: duplicate slashes and "." dropped, ".." KEPT
// (kernel-order resolution applies ".." to the RESOLVED parent, so it
// must survive until the symlink-aware step below, CodeRabbit #112).
std::vector<std::string> split_segments(const std::string& path) {
    std::vector<std::string> segs;
    std::string cur;
    for (const char ch : path) {
        if (ch == '/') {
            if (!cur.empty() && cur != ".") segs.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    if (!cur.empty() && cur != ".") segs.push_back(cur);
    return segs;
}

std::string join_segments(const std::vector<std::string>& segs, bool absolute) {
    std::string out = absolute ? "/" : "";
    for (const std::string& seg : segs) {
        if (!out.empty() && out.back() != '/') out += '/';
        out += seg;
    }
    return out.empty() ? (absolute ? "/" : ".") : out;
}

}  // namespace

std::string canonical_root(const std::string& path) {
    // One consistent representation for the overlap gate (CodeRabbit
    // #112): the longest EXISTING prefix resolves via realpath - with
    // ".." kept in the spelling, because realpath applies it to the
    // RESOLVED parent ("/shares/link/../Photos" where link is a symlink
    // is NOT "/shares/Photos") - and the unresolved tail's segments then
    // fold onto the canonical base: ".." pops one segment, which is safe
    // lexically because the base is already canonical and a nonexistent
    // tail contains no symlinks. A temporarily missing (unmounted) root
    // and an existing symlinked neighbour therefore compare in one form.
    const bool absolute = !path.empty() && path[0] == '/';
    const std::vector<std::string> segs = split_segments(path);
    std::string base;
    size_t kept = segs.size();
    while (true) {
        const std::string cand =
            join_segments({segs.begin(), segs.begin() + kept}, absolute);
        char resolved[PATH_MAX];
        if ((kept > 0 || absolute) &&
            realpath(cand.c_str(), resolved) != nullptr) {
            base = resolved;
            break;
        }
        if (kept == 0) {  // nothing resolves: fold onto the bare anchor
            base = absolute ? "/" : "";
            break;
        }
        --kept;
    }
    std::vector<std::string> out = split_segments(base);
    for (size_t i = kept; i < segs.size(); ++i) {
        if (segs[i] == "..") {
            if (!out.empty()) out.pop_back();
        } else {
            out.push_back(segs[i]);
        }
    }
    return join_segments(out, base.empty() ? absolute : true);
}

bool roots_overlap(const std::string& canonical_a, const std::string& canonical_b) {
    if (canonical_a == canonical_b) return true;
    // Ancestor test with a segment boundary: "/a/b" is an ancestor of
    // "/a/b/c" but not of "/a/bc".
    if (canonical_a.size() < canonical_b.size() &&
        canonical_b.compare(0, canonical_a.size(), canonical_a) == 0 &&
        (canonical_a == "/" || canonical_b[canonical_a.size()] == '/')) {
        return true;
    }
    if (canonical_b.size() < canonical_a.size() &&
        canonical_a.compare(0, canonical_b.size(), canonical_b) == 0 &&
        (canonical_b == "/" || canonical_a[canonical_b.size()] == '/')) {
        return true;
    }
    return false;
}

}  // namespace wfi
