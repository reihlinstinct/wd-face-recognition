#include "cropreserve.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
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

bool ensure_private_dir(const std::string& dir, std::string& err) {
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        err = "cannot create " + dir + ": " + std::strerror(errno);
        return false;
    }
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        err = dir + " exists but is not a directory";
        return false;
    }
    if ((st.st_mode & 0777) != 0700 && chmod(dir.c_str(), 0700) != 0) {
        err = "cannot tighten permissions on " + dir + ": " +
              std::strerror(errno);
        return false;
    }
    if (access(dir.c_str(), W_OK) != 0) {
        err = dir + " is not writable: " + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace wfi
