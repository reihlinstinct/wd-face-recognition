// People clustering over stored embeddings (issue #50, 0.3.0 PR 3b):
// union-find on cosine similarity, same 0.5 threshold as the demo, then a
// single-transaction rewrite via Db::replace_clusters (id/name
// preservation lives there). Reuses stored embeddings per INCREMENTAL.md
// section 5 - no re-decode, no model load.
//
// Cost note: pairing is O(n^2) in face count. That is fine at personal
// NAS scale (a few thousand faces); a larger library needs approximate
// nearest neighbors first - file a follow-up before raising scale.
#pragma once

#include <string>

#include "db.h"

namespace wfi {

struct ClusterCounts {
    long faces = 0;     // faces considered
    long people = 0;    // resulting clusters
    long singletons = 0; // one-face clusters
};

// Recomputes all cluster assignments from stored embeddings. False with
// err on a DB failure. threshold is the cosine-similarity cut (0.5
// matches the demo's MobileFaceNet calibration).
bool recluster(Db& db, double threshold, ClusterCounts& counts,
               std::string& err);

// JSON for the PHP pages (PR 3c): the stripped OS5 PHP has no sqlite
// extension, so the pages shell out to `wdf_faces people|person` and
// render these. json_escape handles quotes, backslashes and control
// chars; UTF-8 (Hebrew names) passes through untouched.
std::string json_escape(const std::string& s);
std::string people_json(const std::vector<PersonSummary>& people);
std::string person_json(const PersonSummary& who,
                        const std::vector<PersonFace>& faces);

}  // namespace wfi
