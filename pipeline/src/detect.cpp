#include "detect.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <net.h>

namespace wfp {

void load_net(ncnn::Net& net, const std::string& dir, const char* name, int threads) {
    // Set before load: ncnn captures num_threads per layer at load time.
    net.opt.num_threads = threads;
    if (net.load_param((dir + "/" + name + ".param").c_str()) != 0 ||
        net.load_model((dir + "/" + name + ".bin").c_str()) != 0) {
        std::fprintf(stderr, "failed to load model %s from %s\n", name, dir.c_str());
        std::exit(1);
    }
}

std::vector<Face> scrfd_detect(ncnn::Net& net, const RgbImage& img) {
    constexpr int kInput = 640;
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(
        img.pixels.data(), ncnn::Mat::PIXEL_RGB, img.w, img.h, kInput, kInput);
    const float mean[3] = {127.5f, 127.5f, 127.5f};
    const float norm[3] = {1 / 128.f, 1 / 128.f, 1 / 128.f};
    in.substract_mean_normalize(mean, norm);

    ncnn::Extractor ex = net.create_extractor();
    ex.input(net.input_names()[0], in);

    std::vector<Face> faces;
    // det_500m's numeric output blob names arrive ordered by stride
    // (score, bbox, kps) - verified live in CI. Element counts are not
    // unique across strides (stride-8 scores and stride-16 boxes are both
    // 12800), so order is the mapping and element counts are the check.
    const std::vector<const char*>& out_names = net.output_names();
    if (out_names.size() != 9) {
        std::fprintf(stderr, "expected 9 SCRFD outputs, got %zu\n", out_names.size());
        std::exit(1);
    }
    ncnn::Mat outm[9];
    for (int oi = 0; oi < 9; ++oi) ex.extract(out_names[oi], outm[oi]);
    for (int si = 0; si < 3; ++si) {
        const int stride = 8 << si;
        const int fmap = kInput / stride;
        const int num_anchors = 2;
        const size_t anchors = (size_t)fmap * fmap * num_anchors;
        ncnn::Mat& scores = outm[si * 3];
        ncnn::Mat& boxes = outm[si * 3 + 1];
        ncnn::Mat& kps = outm[si * 3 + 2];
        if (scores.total() != anchors || boxes.total() != anchors * 4 || kps.total() != anchors * 10) {
            std::fprintf(stderr,
                "SCRFD outputs for stride %d unexpected; want totals %zu/%zu/%zu, got %s=%zu %s=%zu %s=%zu\n",
                stride, anchors, anchors * 4, anchors * 10,
                out_names[si * 3], scores.total(), out_names[si * 3 + 1], boxes.total(),
                out_names[si * 3 + 2], kps.total());
            std::exit(1);
        }
        for (int y = 0; y < fmap; ++y) {
            for (int x = 0; x < fmap; ++x) {
                for (int a = 0; a < num_anchors; ++a) {
                    const int idx = y * fmap * num_anchors + x * num_anchors + a;
                    const float score = ((const float*)scores.data)[idx];
                    if (score < 0.6f) continue;  // slightly above the SCRFD default 0.5: keeps results free of the usual low-confidence texture false positives
                    const float cx = (x + 0.5f) * stride;
                    const float cy = (y + 0.5f) * stride;
                    const float* b = (const float*)boxes.data + (size_t)idx * 4;
                    const float* k = (const float*)kps.data + (size_t)idx * 10;
                    Face face{};
                    face.x0 = cx - b[0] * stride;
                    face.y0 = cy - b[1] * stride;
                    face.x1 = cx + b[2] * stride;
                    face.y1 = cy + b[3] * stride;
                    face.score = score;
                    for (int p = 0; p < 10; p += 2) {
                        // kps are anchor-relative offsets (times stride),
                        // same encoding as the box distances.
                        face.kps[p] = cx + k[p] * stride;
                        face.kps[p + 1] = cy + k[p + 1] * stride;
                    }
                    faces.push_back(face);
                }
            }
        }
    }

    // scale back to original image coordinates
    const float sx = (float)img.w / kInput;
    const float sy = (float)img.h / kInput;
    for (Face& f : faces) {
        f.x0 *= sx; f.x1 *= sx;
        f.y0 *= sy; f.y1 *= sy;
        for (int p = 0; p < 10; p += 2) {
            f.kps[p] *= sx;
            f.kps[p + 1] *= sy;
        }
    }

    // crude NMS
    std::sort(faces.begin(), faces.end(),
              [](const Face& a, const Face& b) { return a.score > b.score; });
    std::vector<Face> kept;
    for (const Face& f : faces) {
        bool dup = false;
        for (const Face& k : kept) {
            const float ix0 = std::max(f.x0, k.x0), iy0 = std::max(f.y0, k.y0);
            const float ix1 = std::min(f.x1, k.x1), iy1 = std::min(f.y1, k.y1);
            const float inter = std::max(0.f, ix1 - ix0) * std::max(0.f, iy1 - iy0);
            const float area_f = (f.x1 - f.x0) * (f.y1 - f.y0);
            const float area_k = (k.x1 - k.x0) * (k.y1 - k.y0);
            if (inter / std::min(area_f, area_k) > 0.4f) { dup = true; break; }
        }
        if (!dup) kept.push_back(f);
    }
    return kept;
}

std::vector<float> arcface_embed(ncnn::Net& net, const RgbImage& chip) {
    ncnn::Mat in = ncnn::Mat::from_pixels(
        chip.pixels.data(), ncnn::Mat::PIXEL_RGB, chip.w, chip.h);
    const float mean[3] = {127.5f, 127.5f, 127.5f};
    const float norm[3] = {1 / 127.5f, 1 / 127.5f, 1 / 127.5f};
    in.substract_mean_normalize(mean, norm);
    ncnn::Extractor ex = net.create_extractor();
    ex.input(net.input_names()[0], in);
    ncnn::Mat out;
    ex.extract(net.output_names()[0], out);
    std::vector<float> emb(out.w);
    for (int i = 0; i < out.w; ++i) emb[i] = out[i];
    return emb;
}

}  // namespace wfp
