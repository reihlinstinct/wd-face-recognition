#pragma once
// Streaming directory walk with mtime-delta diffing against the index
// (docs/INCREMENTAL.md section 4). No whole-library list in RAM.
#include <string>

namespace wfi {

class Db;
struct ScanCounts;

// Walks one root (recursive, sorted entries, symlinked dirs never followed),
// observing each JPEG into the index. Fills counts; returns false on a DB
// error (walk errors on single entries are skipped, not fatal).
bool scan_root(Db& db, long job_id, const std::string& root,
               int max_depth, ScanCounts& counts, std::string& err);

// Canonical form of a root path for the overlap gate (root removal, #102):
// realpath() when the path exists, else a lexical cleanup (duplicate
// slashes, "." and ".." segments, trailing slash). Never empty for a
// nonempty input.
std::string canonical_root(const std::string& path);

// True when two canonical roots are equal or one is an ancestor of the
// other (the rule-9 relation in docs/ROOTS-SETTINGS.md). Used to gate
// unconfigured-root sweeps on the completeness of overlapping walks.
bool roots_overlap(const std::string& canonical_a, const std::string& canonical_b);

}  // namespace wfi
