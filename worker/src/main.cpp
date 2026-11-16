#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kVersion = "0.1.0";

// --help text (issue #131): stdout, exit 0.
constexpr const char* kHelp =
    "usage: wdf-worker [--help | --version]\n"
    "\n"
    "Face-detection worker for the wd-face-recognition app. M1 skeleton:\n"
    "it prints a banner and exits; face detection arrives in milestone 3.\n"
    "\n"
    "  -h, --help   show this help and exit\n"
    "  --version    print the version and exit\n";
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf-worker %s\n", kVersion);
        return 0;
    }
    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::fputs(kHelp, stdout);
        return 0;
    }
    std::printf("wdf-worker %s (M1 skeleton - face detection arrives in milestone 3)\n",
                kVersion);
    return 0;
}
