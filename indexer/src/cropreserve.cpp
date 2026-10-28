#include "cropreserve.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace wfi {

bool reserve_crop_file(const std::string& abs_dir, const std::string& stem,
                       std::string& abs_path, std::string& err) {
    for (int retry = 0; retry < 100; ++retry) {
        const std::string name =
            retry == 0 ? stem + ".jpg"
                       : stem + "_r" + std::to_string(retry) + ".jpg";
        const std::string path = abs_dir + "/" + name;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            ::close(fd);
            abs_path = path;
            return true;
        }
        if (errno != EEXIST) {
            err = "cannot reserve " + path + ": " + std::strerror(errno);
            return false;
        }
    }
    err = "no free crop path under " + abs_dir + " for stem " + stem +
          " (100 collisions)";
    return false;
}

}  // namespace wfi
