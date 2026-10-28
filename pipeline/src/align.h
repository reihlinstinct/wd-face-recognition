// Face-chip extraction (issue #40, PR 2): similarity-transform alignment of
// the SCRFD 5-point landmarks onto the ArcFace 112x112 reference, with the
// demo's margin-crop chip as fallback. Pure math, no ncnn/libjpeg deps.
#pragma once

#include "image.h"

namespace wfp {

// Least-squares 2D similarity transform (rotation + uniform scale +
// translation, no reflection) mapping src -> dst, 5 points each as
// x0,y0,...,x4,y4. Returns false when the source points are degenerate
// (near-zero spread): the caller then falls back to crop_chip.
bool estimate_similarity(const float src[10], const float dst[10], float m[6]);

// Warps img into the ArcFace 112x112 reference frame using the detected
// landmarks kps[10] (x,y x 5, original photo coordinates). Bilinear
// sampling with edge clamping. Empty image when kps are degenerate.
RgbImage align_chip(const RgbImage& img, const float kps[10]);

// The demo's original chip: 112x112 nearest-neighbour resize of the bbox
// plus 20% margin, clamped to the photo. Empty on a degenerate box.
RgbImage crop_chip(const RgbImage& img, float x0, float y0, float x1, float y1);

}  // namespace wfp
