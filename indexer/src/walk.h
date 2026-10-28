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

}  // namespace wfi
