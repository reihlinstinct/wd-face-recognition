// Pipeline unit tests (issue #40, PR 2): alignment transform recovery,
// chip landmark placement, degenerate-input handling, cosine, union-find,
// and a libjpeg encode/decode roundtrip. No ncnn needed. Run: ./pipeline_test
#include <cmath>
#include <cstdio>
#include <string>

#include "../src/align.h"
#include "../src/cluster.h"
#include "../src/jpegio.h"

static int fails = 0;
static void check(bool cond, const char* label) {
    if (!cond) {
        ++fails;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

int main() {
    // estimate_similarity recovers a known rotation+scale+translation.
    {
        const float src[10] = {10, 10, 20, 10, 15, 20, 12, 30, 18, 30};
        const float ca = std::cos(0.3f), sa = std::sin(0.3f), s = 2.5f;
        float dst[10];
        for (int i = 0; i < 5; ++i) {
            dst[i * 2] = s * (ca * src[i * 2] - sa * src[i * 2 + 1]) + 100.f;
            dst[i * 2 + 1] = s * (sa * src[i * 2] + ca * src[i * 2 + 1]) + 50.f;
        }
        float m[6];
        check(wfp::estimate_similarity(src, dst, m), "similarity estimated");
        for (int i = 0; i < 5; ++i) {
            const float px = m[0] * src[i * 2] + m[1] * src[i * 2 + 1] + m[2];
            const float py = m[3] * src[i * 2] + m[4] * src[i * 2 + 1] + m[5];
            check(std::fabs(px - dst[i * 2]) < 0.01f && std::fabs(py - dst[i * 2 + 1]) < 0.01f,
                  "recovered transform reproduces mapping");
        }
        const float deg[10] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};  // zero spread
        check(!wfp::estimate_similarity(deg, dst, m), "degenerate landmarks refused");
    }

    // align_chip: a bright dot at a landmark lands near its reference point.
    {
        wfp::RgbImage img;
        img.w = 200; img.h = 200;
        img.pixels.assign(200 * 200 * 3, 0);
        // Landmarks at a plausible face layout; left-eye dot is white.
        const float kps[10] = {60, 80, 140, 80, 100, 110, 70, 150, 130, 150};
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                for (int c = 0; c < 3; ++c)
                    img.pixels[((size_t)(80 + dy) * 200 + (60 + dx)) * 3 + c] = 255;
        wfp::RgbImage chip = wfp::align_chip(img, kps);
        check(chip.w == 112 && chip.h == 112 && !chip.pixels.empty(), "aligned chip produced");
        // Reference left eye ~ (38.29, 51.70); the bright region should sit
        // within 8px of it.
        float best = 1e9f;
        int bx = -1, by = -1;
        for (int y = 0; y < 112; ++y)
            for (int x = 0; x < 112; ++x)
                if (chip.pixels[((size_t)y * 112 + x) * 3] > 200) { bx = x; by = y; }
        check(bx >= 0, "bright dot survives the warp");
        if (bx >= 0) {
            best = std::hypot(bx - 38.29f, by - 51.70f);
            check(best < 8.f, "landmark maps to its reference location");
        }
        // All-zero kps -> degenerate -> empty chip (caller falls back).
        const float zero[10] = {};
        check(wfp::align_chip(img, zero).pixels.empty(), "zero kps refused");
    }

    // crop_chip: margin + clamp + degenerate rejection (demo's original).
    {
        wfp::RgbImage img;
        img.w = 100; img.h = 100;
        img.pixels.assign(100 * 100 * 3, 128);
        wfp::RgbImage chip = wfp::crop_chip(img, 20, 20, 60, 60);
        check(chip.w == 112 && chip.pixels.size() == 112 * 112 * 3, "crop chip sized");
        check(wfp::crop_chip(img, 60, 60, 20, 20).pixels.empty(), "inverted box refused");
    }

    // cosine + union-find.
    {
        std::vector<float> a = {1, 0, 0}, b = {1, 0, 0}, c = {0, 1, 0}, z = {0, 0, 0};
        check(std::fabs(wfp::cosine(a, b) - 1.0) < 1e-9, "cosine identical = 1");
        check(std::fabs(wfp::cosine(a, c)) < 1e-9, "cosine orthogonal = 0");
        check(wfp::cosine(a, z) == 0, "cosine with zero vector = 0");
        wfp::UnionFind uf(4);
        uf.unite(0, 1); uf.unite(2, 3);
        check(uf.find(0) == uf.find(1) && uf.find(0) != uf.find(2), "union-find merges");
    }

    // JPEG roundtrip: encode a gradient, decode it back, same dimensions.
    {
        const std::string p = "/tmp/wfp-roundtrip.jpg";
        unsigned char grad[64 * 64 * 3];
        for (int i = 0; i < 64 * 64 * 3; ++i) grad[i] = (unsigned char)(i % 251);
        check(wfp::encode_jpeg(grad, 64, 64, p), "jpeg encodes");
        wfp::RgbImage img = wfp::decode_jpeg(p);
        check(img.w == 64 && img.h == 64 && img.pixels.size() == 64 * 64 * 3, "jpeg decodes");
        std::remove(p.c_str());
        check(wfp::decode_jpeg("/tmp/definitely-not-here.jpg").pixels.empty(),
              "missing file decodes to empty");
    }

    if (fails == 0) std::printf("pipeline tests: all passed\n");
    return fails == 0 ? 0 : 1;
}
