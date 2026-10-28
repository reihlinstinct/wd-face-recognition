#include "align.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wfp {

namespace {

// ArcFace 112x112 reference landmarks (insightface src/common/face_align).
const float kArcfaceRef[10] = {
    38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f,
    41.5493f, 92.3655f, 70.7299f, 92.2041f,
};


}  // namespace

bool estimate_similarity(const float src[10], const float dst[10], float m[6]) {
    float scx = 0, scy = 0, dcx = 0, dcy = 0;
    for (int i = 0; i < 5; ++i) {
        scx += src[i * 2]; scy += src[i * 2 + 1];
        dcx += dst[i * 2]; dcy += dst[i * 2 + 1];
    }
    scx /= 5; scy /= 5; dcx /= 5; dcy /= 5;
    double num_a = 0, num_b = 0, den = 0;
    for (int i = 0; i < 5; ++i) {
        const double ux = src[i * 2] - scx, uy = src[i * 2 + 1] - scy;
        const double vx = dst[i * 2] - dcx, vy = dst[i * 2 + 1] - dcy;
        num_a += ux * vx + uy * vy;
        num_b += ux * vy - uy * vx;
        den += ux * ux + uy * uy;
    }
    if (den < 1e-6) return false;  // degenerate landmarks
    const double a = num_a / den, b = num_b / den;
    m[0] = (float)a;  m[1] = (float)-b; m[2] = (float)(dcx - a * scx + b * scy);
    m[3] = (float)b;  m[4] = (float)a;  m[5] = (float)(dcy - b * scx - a * scy);
    return true;
}

RgbImage align_chip(const RgbImage& img, const float kps[10]) {
    float m[6];
    if (!estimate_similarity(kps, kArcfaceRef, m) || img.pixels.empty()) return {};
    // Invert the 2x3 similarity for output->source sampling.
    const float det = m[0] * m[4] - m[1] * m[3];
    if (std::fabs(det) < 1e-12f) return {};
    const float inv[6] = {
        m[4] / det, -m[1] / det, (m[1] * m[5] - m[4] * m[2]) / det,
        -m[3] / det, m[0] / det, (m[3] * m[2] - m[0] * m[5]) / det,
    };
    RgbImage chip;
    chip.w = 112;
    chip.h = 112;
    chip.pixels.resize(112 * 112 * 3);
    for (int y = 0; y < 112; ++y) {
        for (int x = 0; x < 112; ++x) {
            const float sx = inv[0] * x + inv[1] * y + inv[2];
            const float sy = inv[3] * x + inv[4] * y + inv[5];
            // Bilinear sample, clamped to the image edge (a partly-outside
            // warp is still a usable chip; black borders would poison the
            // embedding more than replicated edges do).
            const float cx = std::min(std::max(sx, 0.f), (float)(img.w - 1));
            const float cy = std::min(std::max(sy, 0.f), (float)(img.h - 1));
            const int x0 = (int)cx, y0 = (int)cy;
            const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
            const float fx = cx - x0, fy = cy - y0;
            for (int c = 0; c < 3; ++c) {
                const float p00 = img.pixels[((size_t)y0 * img.w + x0) * 3 + c];
                const float p01 = img.pixels[((size_t)y0 * img.w + x1) * 3 + c];
                const float p10 = img.pixels[((size_t)y1 * img.w + x0) * 3 + c];
                const float p11 = img.pixels[((size_t)y1 * img.w + x1) * 3 + c];
                const float v = p00 * (1 - fx) * (1 - fy) + p01 * fx * (1 - fy) +
                                p10 * (1 - fx) * fy + p11 * fx * fy;
                chip.pixels[((size_t)y * 112 + x) * 3 + c] = (unsigned char)(v + 0.5f);
            }
        }
    }
    return chip;
}

RgbImage crop_chip(const RgbImage& img, float x0f, float y0f, float x1f, float y1f) {
    const float w = x1f - x0f, h = y1f - y0f;
    const int x0 = std::max(0, (int)(x0f - 0.2f * w));
    const int y0 = std::max(0, (int)(y0f - 0.2f * h));
    const int x1 = std::min(img.w, (int)(x1f + 0.2f * w));
    const int y1 = std::min(img.h, (int)(y1f + 0.2f * h));
    // Reject degenerate boxes: raw SCRFD distances can push a corner past the
    // image edge or invert the box; clamping alone leaves an empty crop whose
    // resize math would read outside img.pixels.
    if (x1 <= x0 || y1 <= y0) return {};
    RgbImage chip;
    chip.w = 112;
    chip.h = 112;
    chip.pixels.resize(112 * 112 * 3);
    for (int y = 0; y < 112; ++y) {
        const int sy = y0 + (y * (y1 - y0)) / 112;
        for (int x = 0; x < 112; ++x) {
            const int sx = x0 + (x * (x1 - x0)) / 112;
            std::memcpy(&chip.pixels[((size_t)y * 112 + x) * 3],
                        &img.pixels[((size_t)sy * img.w + sx) * 3], 3);
        }
    }
    return chip;
}

}  // namespace wfp
