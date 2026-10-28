#pragma once
// Indexer config: simple key=value file, '#' comments. Keys:
//   roots     comma-separated absolute scan roots (under /shares/)
//   data_dir  where index.db and crops live (RAID volume)
#include <string>
#include <vector>

namespace wfi {

struct Config {
    std::vector<std::string> roots;
    std::string data_dir;
};

// Parses the config file. Returns false on unreadable file or missing keys.
bool load_config(const std::string& path, Config& out, std::string& err);

}  // namespace wfi
