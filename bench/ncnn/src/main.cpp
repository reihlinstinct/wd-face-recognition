// wdf-bench-ncnn: M2 pipeline spike (issue #3).
// decode (libjpeg-turbo) -> SCRFD-500M detect -> MobileFaceNet embed,
// all on baseline x86-64 (no AVX), measuring latency per stage and peak RSS.
// Subcommands:
//   pipeline <photo.jpg> <models_dir> <threads> <iters>
//   embed    <crop112.jpg> <models_dir> <iters>
//   heic     <photo.heic> <iters>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>

#include <jpeglib.h>
#include <libheif/heif.h>
#include <net.h>

namespace {

struct RgbImage {
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;  // RGB interleaved
};

RgbImage decode_jpeg(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
    jpeg_decompress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    RgbImage img;
    img.w = static_cast<int>(cinfo.output_width);
    img.h = static_cast<int>(cinfo.output_height);
    img.pixels.resize(static_cast<size_t>(img.w) * img.h * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char* row = img.pixels.data() + static_cast<size_t>(cinfo.output_scanline) * img.w * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(f);
    return img;
}

struct Face {
    float x0, y0, x1, y1, score;
    float kps[10];
};

// Minimal SCRFD postprocess: 3 strides (8/16/32), score threshold, NMS.
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
                    if (score < 0.5f) continue;
                    const float cx = (x + 0.5f) * (8 << si);
                    const float cy = (y + 0.5f) * (8 << si);
                    const float* b = (const float*)boxes.data + (size_t)idx * 4;
                    Face face{};
                    face.x0 = cx - b[0] * (8 << si);
                    face.y0 = cy - b[1] * (8 << si);
                    face.x1 = cx + b[2] * (8 << si);
                    face.y1 = cy + b[3] * (8 << si);
                    face.score = score;
                    if (!kps.empty()) {
                        const float* k = (const float*)kps.data + (size_t)idx * 10;
                        for (int j = 0; j < 10; ++j) face.kps[j] = k[j] * (8 << si) + ((j % 2 == 0) ? cx : cy);
                    }
                    faces.push_back(face);
                }
            }
        }
    }
    // NMS (IoU 0.4)
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    std::vector<Face> kept;
    for (const Face& f : faces) {
        bool drop = false;
        for (const Face& k : kept) {
            const float ix = std::max(0.f, std::min(f.x1, k.x1) - std::max(f.x0, k.x0));
            const float iy = std::max(0.f, std::min(f.y1, k.y1) - std::max(f.y0, k.y0));
            const float inter = ix * iy;
            const float area = (f.x1 - f.x0) * (f.y1 - f.y0) + (k.x1 - k.x0) * (k.y1 - k.y0) - inter;
            if (area > 0 && inter / area > 0.4f) { drop = true; break; }
        }
        if (!drop) kept.push_back(f);
    }
    return kept;
}

std::vector<float> arcface_embed(ncnn::Net& net, const unsigned char* crop112) {
    ncnn::Mat in = ncnn::Mat::from_pixels(crop112, ncnn::Mat::PIXEL_RGB, 112, 112);
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

void load_net(ncnn::Net& net, const std::string& dir, const char* name, int threads) {
    // Set before load: ncnn captures num_threads per layer at load time and
    // warns ("will use load-time value") if it changes afterwards.
    net.opt.num_threads = threads;
    if (net.load_param((dir + "/" + name + ".param").c_str()) != 0 ||
        net.load_model((dir + "/" + name + ".bin").c_str()) != 0) {
        std::fprintf(stderr, "failed to load model %s from %s\n", name, dir.c_str());
        std::exit(1);
    }
}

long peak_rss_kib() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int cmd_pipeline(const char* photo, const char* models_dir, int threads, int iters) {
    ncnn::Net det, rec;
    load_net(det, models_dir, "scrfd_500m", threads);
    load_net(rec, models_dir, "mobilefacenet", threads);

    for (int it = 0; it < iters; ++it) {
        const auto t0 = Clock::now();
        RgbImage img = decode_jpeg(photo);
        const double t_decode = ms_since(t0);

        const auto t1 = Clock::now();
        std::vector<Face> faces = scrfd_detect(det, img);
        const double t_detect = ms_since(t1);

        double t_embed = 0;
        if (!faces.empty()) {
            // M2 latency spike: content of the crop does not affect embedding
            // latency, so a clamped 112x112 window of the photo stands in for
            // the aligned face chip (real alignment lands with the indexer).
            const Face& f = faces[0];
            (void)f;
            std::vector<unsigned char> crop(112 * 112 * 3);
            for (int y = 0; y < 112; ++y) {
                const int sy = std::min(y, img.h - 1);
                for (int x = 0; x < 112; ++x) {
                    const int sx = std::min(x, img.w - 1);
                    std::memcpy(&crop[(y * 112 + x) * 3], &img.pixels[(sy * img.w + sx) * 3], 3);
                }
            }
            const auto t2 = Clock::now();
            std::vector<float> emb = arcface_embed(rec, crop.data());
            t_embed = ms_since(t2);
            if (it == 0) std::printf("embed_dim=%zu emb0=%.4f\n", emb.size(), emb.empty() ? 0.f : emb[0]);
        }
        std::printf("iter=%d decode_ms=%.1f detect_ms=%.1f embed_ms=%.1f faces=%zu total_ms=%.1f\n",
                    it, t_decode, t_detect, t_embed, faces.size(), ms_since(t0));
    }
    std::printf("threads=%d peak_rss_mib=%.1f\n", threads, peak_rss_kib() / 1024.0);
    return 0;
}

int cmd_embed(const char* crop, const char* models_dir, int iters) {
    ncnn::Net rec;
    load_net(rec, models_dir, "mobilefacenet", 1);
    RgbImage img = decode_jpeg(crop);
    if (img.w != 112 || img.h != 112) {
        std::fprintf(stderr, "crop must be 112x112, got %dx%d\n", img.w, img.h);
        return 1;
    }
    double total = 0;
    for (int it = 0; it < iters; ++it) {
        const auto t0 = Clock::now();
        std::vector<float> emb = arcface_embed(rec, img.pixels.data());
        total += ms_since(t0);
        if (it == 0) std::printf("embed_dim=%zu\n", emb.size());
    }
    std::printf("embed_ms_avg=%.2f iters=%d peak_rss_mib=%.1f\n", total / iters, iters, peak_rss_kib() / 1024.0);
    return 0;
}

int cmd_heic(const char* path, int iters) {
    for (int it = 0; it < iters; ++it) {
        const auto t0 = Clock::now();
        heif_context* ctx = heif_context_alloc();
        heif_error err = heif_context_read_from_file(ctx, path, nullptr);
        if (err.code != heif_error_Ok) {
            std::fprintf(stderr, "heif read failed: %s\n", err.message);
            heif_context_free(ctx);
            return 1;
        }
        heif_image_handle* handle = nullptr;
        heif_context_get_primary_image_handle(ctx, &handle);
        heif_image* decoded = nullptr;
        err = heif_decode_image(handle, &decoded, heif_colorspace_RGB,
                                heif_chroma_interleaved_RGB, nullptr);
        if (err.code != heif_error_Ok) {
            std::fprintf(stderr, "heif decode failed: %s\n", err.message);
            heif_image_handle_release(handle);
            heif_context_free(ctx);
            return 1;
        }
        const int w = heif_image_get_width(decoded, heif_channel_interleaved);
        const int h = heif_image_get_height(decoded, heif_channel_interleaved);
        std::printf("iter=%d heic_decode_ms=%.1f w=%d h=%d\n", it, ms_since(t0), w, h);
        heif_image_release(decoded);
        heif_image_handle_release(handle);
        heif_context_free(ctx);
    }
    std::printf("peak_rss_mib=%.1f\n", peak_rss_kib() / 1024.0);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <pipeline|embed> [args]\n", argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "pipeline" && argc >= 6) return cmd_pipeline(argv[2], argv[3], std::atoi(argv[4]), std::atoi(argv[5]));
    if (cmd == "embed" && argc >= 5) return cmd_embed(argv[2], argv[3], std::atoi(argv[4]));
    if (cmd == "heic" && argc >= 4) return cmd_heic(argv[2], std::atoi(argv[3]));
    std::fprintf(stderr, "bad args\n");
    return 2;
}
