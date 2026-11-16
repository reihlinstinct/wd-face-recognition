// SCRFD detection + MobileFaceNet embedding via ncnn (issue #40, PR 2).
// Moved out of demo/src/main.cpp; landmarks are now populated (the demo
// never used them, the aligned chip path does).
#pragma once

#include <string>
#include <vector>

#include "image.h"

namespace ncnn { class Net; }

namespace wfp {

struct Face {
    float x0, y0, x1, y1;
    float score;
    float kps[10];  // 5 landmarks (x,y), original photo coordinates
};

// Loads <dir>/<name>.{param,bin} into net. Exits the process on failure,
// same contract as the demo (a daemon without models is useless anyway).
void load_net(ncnn::Net& net, const std::string& dir, const char* name, int threads);

// SCRFD-500M detect: 3 strides, score threshold 0.6, NMS. Exits on an
// unexpected output layout (model mismatch is not recoverable mid-run).
std::vector<Face> scrfd_detect(ncnn::Net& net, const RgbImage& img);

// MobileFaceNet ArcFace embedding of a 112x112 chip (any RGB size accepted;
// the net resizes internally only when built for it - feed it 112x112).
std::vector<float> arcface_embed(ncnn::Net& net, const RgbImage& chip);

}  // namespace wfp
