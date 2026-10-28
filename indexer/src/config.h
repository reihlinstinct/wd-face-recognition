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

// Parses a --threshold CLI value: strtod with full consume, finite, and
// within the cosine range [-1, 1] (issue #67). False on anything else.
bool parse_threshold(const char* s, double& out);

// wdf_faces command-line options (after the subcommand).
struct CliOptions {
    std::string config_path;
    std::string models_override;
    long limit = 500;        // per-invocation cap
    int threads_override = 0;
    double threshold = 0.5;  // demo-calibrated cosine cut
};

// Parses argv[first..] as flag/value pairs. Every flag needs a value and
// must be known: unknown or incomplete options are rejected (issue #68)
// BEFORE the caller opens the index.
bool parse_cli_options(int argc, char** argv, int first, CliOptions& out,
                       std::string& err);

}  // namespace wfi
