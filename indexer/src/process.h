// Face-processing stage (issue #40 PR 2): picks photos the walk queued,
// runs the shared pipeline (decode -> SCRFD -> align -> MobileFaceNet),
// and writes faces + embeddings + crops into the index. Invalidation per
// docs/INCREMENTAL.md: a re-queued photo's old faces/crops are replaced.
#pragma once

#include <string>

namespace wfi {

class Db;

struct ProcessCounts {
    long processed = 0;   // photos that went through the pipeline
    long faces = 0;       // face rows written
    long failed = 0;      // photos marked failed (undecodable etc)
    long requeued = 0;    // changed mid-processing; left queued for next run
    long remaining = 0;   // still queued after this run (--limit reached)
};

// Processes up to `limit` queued photos. Crops land under
// <data_dir>/crops/p<photo_id>_f<n>.jpg. Returns false only on a DB or
// model-loading error; single-photo problems mark that photo failed and
// processing continues.
bool process_queued(Db& db, const std::string& models_dir,
                    const std::string& data_dir, int threads, long limit,
                    ProcessCounts& counts, std::string& err);

}  // namespace wfi
