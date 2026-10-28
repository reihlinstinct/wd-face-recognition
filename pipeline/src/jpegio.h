// JPEG decode/encode via libjpeg (moved out of demo/src/main.cpp so the
// indexer and the demo share one path, issue #40 PR 2).
#pragma once

#include <string>

#include "image.h"

namespace wfp {

// Decodes a JPEG to RGB. Returns an empty image (pixels.empty()) on any
// failure: a corrupt or CMYK file must never be fatal to a library scan.
RgbImage decode_jpeg(const std::string& path);

// Encodes RGB to a JPEG file (quality 92). False on open/encode failure.
bool encode_jpeg(const unsigned char* rgb, int w, int h, const std::string& path);

}  // namespace wfp
