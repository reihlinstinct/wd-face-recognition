// Recursive photo-folder scan for the on-device demo (issue #24).
// Deliberately free of ncnn/ncnn-adjacent dependencies so it can be unit
// tested without the inference stack.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace wdf {

struct ScanOptions {
    int max_depth = 8;               // root is depth 0; its entries are depth 1
    std::size_t max_photos = 1000;   // per-run cap so a huge library can't hog the NAS
    // Global work budget: total directory entries examined across the whole
    // walk. The photo cap alone does not bound a tree full of non-JPEG files.
    std::size_t max_entries = 200000;
};

struct ScanResult {
    std::vector<std::string> photos;  // paths relative to root, sorted, deduplicated
    bool truncated_photos = false;    // max_photos hit: more photos exist below root
    bool truncated_depth = false;     // max_depth hit: deeper folders were skipped
    std::size_t entries_examined = 0; // directory entries looked at (excludes . and ..)
    bool truncated_entries = false;   // max_entries hit: the walk stopped early
};

// Walks root recursively collecting *.jpg/*.jpeg (case-insensitive).
// Never follows symlinks (a symlinked dir could escape the share or loop);
// only real directories are descended into and only regular files collected.
ScanResult scan_photos(const std::string& root, const ScanOptions& opts = {});

// True for names ending .jpg / .jpeg, case-insensitive.
bool ends_jpeg(const std::string& name);

}  // namespace wdf
