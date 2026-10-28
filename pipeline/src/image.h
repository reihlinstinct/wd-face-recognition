// Shared decoded-image type for the face pipeline (issue #40, PR 2).
// Kept dependency-free so pure-math units (align, cluster) can be unit
// tested without libjpeg or ncnn.
#pragma once

#include <vector>

namespace wfp {

struct RgbImage {
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;  // w*h*3, row-major RGB
};

}  // namespace wfp
