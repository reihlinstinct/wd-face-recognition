#include "config.h"

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

}  // namespace wfi
