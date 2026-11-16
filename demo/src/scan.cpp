#include "scan.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <vector>
#include <sys/stat.h>

namespace wdf {

bool ends_jpeg(const std::string& n) {
    std::string lower = n;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    // Require a non-empty basename before the extension (".jpg" alone is a
    // hidden file with no extension, matching the Python pre-filter's rule).
    return (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".jpg") == 0) ||
           (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0);
}

namespace {

void walk(const std::string& abs, const std::string& rel, int depth,
          const ScanOptions& opts, ScanResult& out) {
    // Once truncation is proven (another JPEG past the cap, or the work
    // budget is spent) further lstat/opendir calls buy nothing: stop. Note
    // there is no early return for merely REACHING the photo cap -
    // truncated_photos is only set when a real JPEG past the cap is seen.
    if (out.truncated_photos || out.truncated_entries) return;
    DIR* d = opendir(abs.c_str());
    if (!d) return;  // unreadable folder: skip, don't fail the whole run
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (out.entries_examined >= opts.max_entries) {
            out.truncated_entries = true;
            break;
        }
        ++out.entries_examined;
        names.push_back(name);
    }
    closedir(d);
    // Sort entries BEFORE traversal: readdir order is filesystem-dependent,
    // and sorting only the final output would leave WHICH photos a capped
    // run selects nondeterministic.
    std::sort(names.begin(), names.end());
    // Entries already collected above have consumed budget, so they are
    // classified normally even when the ENTRY budget was hit mid-readdir
    // (the walk() entry guard keeps recursion from examining anything new).
    // Once photo truncation is proven, though, stop immediately.
    for (const std::string& name : names) {
        if (out.truncated_photos) break;
        const std::string child_abs = abs + "/" + name;
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        struct stat st;
        if (lstat(child_abs.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {  // lstat: symlinked dirs never match, never followed
            if (depth >= opts.max_depth) {
                out.truncated_depth = true;
            } else {
                walk(child_abs, child_rel, depth + 1, opts, out);
            }
        } else if (S_ISREG(st.st_mode) && ends_jpeg(name)) {
            if (out.photos.size() >= opts.max_photos) {
                // A real photo past the cap: evidence that more photos exist.
                out.truncated_photos = true;
            } else {
                out.photos.push_back(child_rel);
            }
        }
    }
}

}  // namespace

ScanResult scan_photos(const std::string& root, const ScanOptions& opts) {
    ScanResult out;
    walk(root, "", 0, opts, out);
    std::sort(out.photos.begin(), out.photos.end());
    out.photos.erase(std::unique(out.photos.begin(), out.photos.end()),
                     out.photos.end());
    return out;
}

}  // namespace wdf
