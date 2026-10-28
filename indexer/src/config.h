#pragma once
// Indexer config: simple key=value file, '#' comments. Keys:
//   roots       comma-separated absolute scan roots (under /shares/)
//   data_dir    where index.db and crops live (RAID volume)
//   models_dir  SCRFD/MobileFaceNet .param/.bin dir (wdf_faces only)
//   threads     ncnn thread count for wdf_faces (optional, default 2)
#include <string>
#include <vector>

namespace wfi {

struct Config {
    std::vector<std::string> roots;
    std::string data_dir;
    std::string models_dir;  // empty when unset: wdf_faces refuses to run
    int threads = 2;         // PR4100 N3710: 4 weak cores, keep headroom
};

// Parses the config file. Returns false on unreadable file or missing keys.
bool load_config(const std::string& path, Config& out, std::string& err);

}  // namespace wfi
