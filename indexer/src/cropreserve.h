// Atomic crop-path reservation (CodeRabbit #45): two concurrent
// `wdf_faces process` runs that start in the same second derive the same
// crop path (run_stamp has one-second resolution), so a path must be
// claimed before any bytes are written. A run only ever unlinks files it
// reserved itself, so a loser can never delete the winner's committed
// crop.
#pragma once

#include <string>

namespace wfi {

// Claims a fresh file under abs_dir, trying "<stem>.jpg" then
// "<stem>_r1.jpg" .. "<stem>_r99.jpg", each opened O_WRONLY|O_CREAT|O_EXCL
// with mode 0600. On success abs_path is the reserved path: the file
// exists, is empty, and no other run can claim it (a later encoder may
// safely truncate and rewrite it). Returns false when all 100 candidates
// exist or the directory is unusable; err says which.
bool reserve_crop_file(const std::string& abs_dir, const std::string& stem,
                       std::string& abs_path, std::string& err);

// Makes dir exist with owner-only access: creates it 0700 when missing,
// tightens a pre-existing directory's mode to 0700 (review #45: a wider
// existing dir must not be trusted), and verifies it is a writable
// directory. False with err on any failure.
bool ensure_private_dir(const std::string& dir, std::string& err);

}  // namespace wfi
