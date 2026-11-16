// Link stub for the wdf_faces CLI dispatch test binary (issue #65): the
// dispatch tests never reach real processing, but faces_main.cpp
// references wfi::process_queued, so the link needs a definition that
// does not pull in ncnn/libjpeg.
#include "process.h"

namespace wfi {

bool process_queued(Db&, const std::string&, const std::string&, int, long,
                    ProcessCounts&, std::string& err) {
    err = "process_queued stub: the CLI test binary cannot process";
    return false;
}

}  // namespace wfi
