#include "config.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace wfi {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool load_config(const std::string& path, Config& out, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open config: " + path;
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (key == "roots") {
            std::stringstream ss(val);
            std::string item;
            while (std::getline(ss, item, ',')) {
                item = trim(item);
                if (!item.empty()) out.roots.push_back(item);
            }
        } else if (key == "data_dir") {
            out.data_dir = val;
        } else if (key == "models_dir") {
            out.models_dir = val;
        } else if (key == "threads") {
            try {
                int n = std::stoi(val);
                if (n >= 1 && n <= 8) out.threads = n;  // beyond 8 helps nothing on this class of NAS
            } catch (...) {
                // Unparseable threads= keeps the default; a bad value must
                // not break the scan config too.
            }
        }
    }
    if (out.roots.empty()) {
        err = "config has no roots= (nothing is scanned until roots are chosen)";
        return false;
    }
    if (out.data_dir.empty()) {
        err = "config has no data_dir=";
        return false;
    }
    return true;
}

bool parse_threshold(const char* s, double& out) {
    if (s == nullptr || *s == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (!std::isfinite(v) || v < -1.0 || v > 1.0) return false;
    out = v;
    return true;
}

// strtol with full consume and range check, for the integer options
// (issue #69): atol accepted trailing junk and wrapped out-of-range input.
static bool parse_long_range(const char* s, long lo, long hi, long& out) {
    if (s == nullptr || *s == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    if (v < lo || v > hi) return false;
    out = v;
    return true;
}

bool parse_cli_options(int argc, char** argv, int first, bool is_cluster,
                       CliOptions& out, std::string& err) {
    for (int i = first; i < argc; i += 2) {
        const char* flag = argv[i];
        if (i + 1 >= argc) {
            err = std::string("option ") + flag + " needs a value";
            return false;
        }
        const char* val = argv[i + 1];
        const bool known =
            std::strcmp(flag, "--config") == 0 ||
            std::strcmp(flag, "--models") == 0 ||
            std::strcmp(flag, "--limit") == 0 ||
            std::strcmp(flag, "--threads") == 0 ||
            std::strcmp(flag, "--threshold") == 0;
        if (!known) {
            err = std::string("unknown option '") + flag + "'";
            return false;
        }
        const bool allowed =
            std::strcmp(flag, "--config") == 0 ||
            (is_cluster && std::strcmp(flag, "--threshold") == 0) ||
            (!is_cluster && std::strcmp(flag, "--threshold") != 0);
        if (!allowed) {
            err = std::string("option '") + flag +
                  (is_cluster ? "' is not valid for cluster"
                              : "' is not valid for process");
            return false;
        }
        if (std::strcmp(flag, "--config") == 0) out.config_path = val;
        else if (std::strcmp(flag, "--models") == 0)
            out.models_override = val;
        else if (std::strcmp(flag, "--limit") == 0) {
            // 1..100000: 0/negative must not become an unbounded run;
            // the cap keeps a runaway library batched.
            if (!parse_long_range(val, 1, 100000, out.limit)) {
                err = std::string("bad --limit '") + val +
                      "': want an integer in [1, 100000]";
                return false;
            }
        } else if (std::strcmp(flag, "--threads") == 0) {
            long t = 0;
            // 1..32: the N3710 has 4 weak cores; 32 is already far past
            // anything useful, but a typo must not ask for thousands.
            if (!parse_long_range(val, 1, 32, t)) {
                err = std::string("bad --threads '") + val +
                      "': want an integer in [1, 32]";
                return false;
            }
            out.threads_override = (int)t;
        } else if (std::strcmp(flag, "--threshold") == 0) {
            if (!parse_threshold(val, out.threshold)) {
                err = std::string("bad --threshold '") + val +
                      "': want a finite number in [-1, 1]";
                return false;
            }
        }
    }
    return true;
}

}  // namespace wfi
