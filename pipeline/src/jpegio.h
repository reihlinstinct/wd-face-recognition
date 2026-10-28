// JPEG decode/encode via libjpeg (moved out of demo/src/main.cpp so the
// indexer and the demo share one path, issue #40 PR 2).
#pragma once

#include <cstdint>
#include <string>

#include "image.h"

namespace wfp {

// Default decode pixel cap (independent review #45): 50MP of RGB is 150MB;
// the PR4100 has 1GB RAM and a scan must never let one photo squeeze out
// everything else. Photos above the cap come back empty with
// *reason = "too large"; corrupt/unsupported files give "decode".
constexpr int64_t kDefaultMaxPixels = 50000000;

// Decodes a JPEG to RGB. Returns an empty image on any failure - a bad
// file must never be fatal to a library scan. When reason is non-null it
// receives a short machine-stable cause ("too large" / "decode").
RgbImage decode_jpeg(const std::string& path, std::string* reason = nullptr,
                     int64_t max_pixels = kDefaultMaxPixels);

// Encodes RGB to a JPEG file (quality 92). False on open/encode failure.
bool encode_jpeg(const unsigned char* rgb, int w, int h, const std::string& path);

}  // namespace wfp
