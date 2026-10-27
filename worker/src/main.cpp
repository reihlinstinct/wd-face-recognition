#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kVersion = "0.1.0";
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf-worker %s\n", kVersion);
        return 0;
    }
    std::printf("wdf-worker %s (M1 skeleton - face detection arrives in milestone 3)\n",
                kVersion);
    return 0;
}
