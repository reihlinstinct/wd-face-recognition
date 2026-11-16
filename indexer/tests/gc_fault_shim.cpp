// Synthetic fault shim used only by test_faces_cli.py, never production.
// Make a failed unlink the final entry, independent of readdir order or UID.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <cstdio>
#include <unistd.h>

static bool target_fd(int fd) {
    const char* expected = std::getenv("WDF_TEST_CROPS");
    if (!expected) return false;
    char proc[64], path[4096];
    std::snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    const ssize_t n = readlink(proc, path, sizeof(path) - 1);
    if (n < 0) return false;
    path[n] = '\0';
    return std::strcmp(path, expected) == 0;
}

extern "C" struct dirent* readdir(DIR* dir) {
    using Real = struct dirent* (*)(DIR*);
    static Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "readdir"));
    if (!target_fd(dirfd(dir))) return real(dir);
    static bool yielded = false;
    static struct dirent entry = {};
    if (!yielded) {
        yielded = true;
        std::strcpy(entry.d_name, "stuck.jpg");
        return &entry;
    }
    // Clean EOF must leave errno alone, just as POSIX permits.
    if (std::getenv("WDF_TEST_READ_ERROR")) errno = EIO;
    return nullptr;
}

extern "C" int unlinkat(int fd, const char* name, int flags) {
    using Real = int (*)(int, const char*, int);
    static Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "unlinkat"));
    if (target_fd(fd) && std::strcmp(name, "stuck.jpg") == 0) {
        errno = EACCES;
        return -1;
    }
    return real(fd, name, flags);
}
