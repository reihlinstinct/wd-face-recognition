// wdf_demo: first visible end-to-end slice (issue #18).
// Scans a folder of JPEGs, detects faces (SCRFD-500M), embeds them
// (MobileFaceNet ArcFace), clusters by cosine similarity, and writes a
// mobile-friendly HTML contact sheet grouped by person.
//
// Usage: wdf_demo <photos_dir> <models_dir> <out_dir> [threads]
//
// Demo scope: JPEG only, no alignment, threshold clustering. The pipeline
// pieces are adapted from the M2 ncnn spike (bench/ncnn, issue #3) and will
// fold into the real indexer (M3) once the design catches up.

#include <net.h>

#include <jpeglib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <vector>

namespace {

// libjpeg's default error_exit calls exit(): one corrupt or CMYK JPEG would
// kill the whole demo run. Recover with setjmp/longjmp and skip the file.
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
};

extern "C" void jpeg_fatal_recover(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->msg);
    longjmp(err->jb, 1);
}

std::string html_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '\'': o += "&#39;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

struct RgbImage {
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
};

RgbImage decode_jpeg(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fatal_recover;
    // Only POD locals may live across this boundary: longjmp bypasses C++
    // destructors, so the pixel buffer is malloc'd here and wrapped in the
    // RgbImage vector only after jpeg_destroy_decompress has run. Locals
    // modified after setjmp are volatile so their values survive the jump.
    unsigned char* volatile buf = nullptr;
    size_t buf_size = 0;
    int w = 0, h = 0;
    bool volatile created = false;
    if (setjmp(jerr.jb)) {
        std::fprintf(stderr, "jpeg decode error in %s: %s\n", path.c_str(), jerr.msg);
        if (created) jpeg_destroy_decompress(&cinfo);
        std::free(buf);
        std::fclose(f);
        return {};
    }
    jpeg_create_decompress(&cinfo);
    created = true;
    jpeg_stdio_src(&cinfo, f);
    if (jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK) {
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        w = (int)cinfo.output_width;
        h = (int)cinfo.output_height;
        buf_size = (size_t)w * h * 3;
        buf = (unsigned char*)std::malloc(buf_size);
        if (buf) {
            while (cinfo.output_scanline < cinfo.output_height) {
                JSAMPROW row = buf + (size_t)cinfo.output_scanline * (size_t)w * 3;
                jpeg_read_scanlines(&cinfo, &row, 1);
            }
            jpeg_finish_decompress(&cinfo);
        }
    }
    jpeg_destroy_decompress(&cinfo);
    created = false;
    std::fclose(f);
    RgbImage img;
    if (buf) {
        img.w = w;
        img.h = h;
        img.pixels.assign(buf, buf + buf_size);
        std::free(buf);
    }
    return img;
}

bool encode_jpeg(const unsigned char* rgb, int w, int h, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    jpeg_compress_struct cinfo{};
    JpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fatal_recover;
    // setjmp precedes jpeg_create_compress: creation itself can invoke the
    // error handler. Only POD locals live across the boundary.
    bool volatile created = false;
    if (setjmp(jerr.jb)) {
        std::fprintf(stderr, "jpeg encode error for %s: %s\n", path.c_str(), jerr.msg);
        if (created) jpeg_destroy_compress(&cinfo);
        std::fclose(f);
        return false;
    }
    jpeg_create_compress(&cinfo);
    created = true;
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 92, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<unsigned char*>(rgb) + (size_t)cinfo.next_scanline * w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(f);
    return true;
}

struct Face {
    float x0, y0, x1, y1;
    float score;
    float kps[10];
    std::vector<float> emb;
};

// Minimal SCRFD postprocess: 3 strides (8/16/32), score threshold, NMS.
// Output mapping is by element count (det_500m has numeric blob names).
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
                    if (score < 0.6f) continue;  // slightly above the SCRFD default 0.5: keeps the demo HTML free of the usual low-confidence texture false positives
                    const float cx = (x + 0.5f) * stride;
                    const float cy = (y + 0.5f) * stride;
                    const float* b = (const float*)boxes.data + (size_t)idx * 4;
                    Face face{};
                    face.x0 = cx - b[0] * stride;
                    face.y0 = cy - b[1] * stride;
                    face.x1 = cx + b[2] * stride;
                    face.y1 = cy + b[3] * stride;
                    face.score = score;
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

void load_net(ncnn::Net& net, const std::string& dir, const char* name, int threads) {
    // Set before load: ncnn captures num_threads per layer at load time.
    net.opt.num_threads = threads;
    if (net.load_param((dir + "/" + name + ".param").c_str()) != 0 ||
        net.load_model((dir + "/" + name + ".bin").c_str()) != 0) {
        std::fprintf(stderr, "failed to load model %s from %s\n", name, dir.c_str());
        std::exit(1);
    }
}

// Crop a 112x112 face chip with 20% margin, clamped to the photo.
RgbImage face_chip(const RgbImage& img, const Face& f) {
    const float w = f.x1 - f.x0, h = f.y1 - f.y0;
    const int x0 = std::max(0, (int)(f.x0 - 0.2f * w));
    const int y0 = std::max(0, (int)(f.y0 - 0.2f * h));
    const int x1 = std::min(img.w, (int)(f.x1 + 0.2f * w));
    const int y1 = std::min(img.h, (int)(f.y1 + 0.2f * h));
    // Reject degenerate boxes: raw SCRFD distances can push a corner past the
    // image edge or invert the box; clamping alone leaves an empty crop whose
    // resize math would read outside img.pixels.
    if (x1 <= x0 || y1 <= y0) return {};
    RgbImage chip;
    chip.w = 112;
    chip.h = 112;
    chip.pixels.resize(112 * 112 * 3);
    // nearest-neighbour resize of the clamped crop (demo-grade; the indexer
    // gets real alignment)
    for (int y = 0; y < 112; ++y) {
        const int sy = y0 + (y * (y1 - y0)) / 112;
        for (int x = 0; x < 112; ++x) {
            const int sx = x0 + (x * (x1 - x0)) / 112;
            std::memcpy(&chip.pixels[(y * 112 + x) * 3],
                        &img.pixels[(sy * img.w + sx) * 3], 3);
        }
    }
    return chip;
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += (double)a[i] * b[i];
        na += (double)a[i] * a[i];
        nb += (double)b[i] * b[i];
    }
    if (na == 0 || nb == 0) return 0;
    return dot / std::sqrt(na * nb);
}

struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) {
        for (size_t i = 0; i < n; ++i) parent[i] = (int)i;
    }
    int find(int x) { return parent[x] == x ? x : (parent[x] = find(parent[x])); }
    void unite(int a, int b) { parent[find(a)] = find(b); }
};

bool ends_jpeg(const std::string& n) {
    std::string lower = n;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    return lower.size() >= 4 &&
           (lower.compare(lower.size() - 4, 4, ".jpg") == 0 ||
            (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_demo 0.2.0 (SCRFD-500M + MobileFaceNet, ncnn)\n");
        return 0;
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <photos_dir> <models_dir> <out_dir> [threads]\n", argv[0]);
        return 2;
    }
    const std::string photos_dir = argv[1];
    const std::string models_dir = argv[2];
    const std::string out_dir = argv[3];
    const int threads = argc > 4 ? std::atoi(argv[4]) : 2;
    constexpr double kClusterThreshold = 0.5;

    ncnn::Net det, rec;
    load_net(det, models_dir, "scrfd_500m", threads);
    load_net(rec, models_dir, "mobilefacenet", threads);

    mkdir(out_dir.c_str(), 0777);
    mkdir((out_dir + "/crops").c_str(), 0777);

    std::vector<std::string> photos;
    DIR* d = opendir(photos_dir.c_str());
    if (!d) {
        std::fprintf(stderr, "cannot open %s\n", photos_dir.c_str());
        return 1;
    }
    while (dirent* e = readdir(d)) {
        if (ends_jpeg(e->d_name)) photos.push_back(e->d_name);
    }
    closedir(d);
    std::sort(photos.begin(), photos.end());
    std::printf("photos=%zu\n", photos.size());

    struct Rec {
        std::string photo;
        std::string crop;
        std::vector<float> emb;
    };
    std::vector<Rec> recs;
    size_t face_total = 0;
    for (const std::string& name : photos) {
        RgbImage img = decode_jpeg(photos_dir + "/" + name);
        if (img.pixels.empty()) {
            std::fprintf(stderr, "skip undecodable %s\n", name.c_str());
            continue;
        }
        std::vector<Face> faces = scrfd_detect(det, img);
        int fi = 0;
        for (const Face& f : faces) {
            RgbImage chip = face_chip(img, f);
            if (chip.pixels.empty()) continue;  // degenerate box, rejected by face_chip
            Rec r;
            r.photo = name;
            r.crop = "crops/f" + std::to_string(recs.size()) + ".jpg";
            r.emb = arcface_embed(rec, chip);
            if (!encode_jpeg(chip.pixels.data(), chip.w, chip.h, out_dir + "/" + r.crop)) {
                std::fprintf(stderr, "crop write failed for %s\n", r.crop.c_str());
                continue;
            }
            recs.push_back(std::move(r));
            ++fi;
        }
        // count kept crops, not raw detections: the header total and the CI
        // detection grep must agree with what the HTML grid actually shows
        face_total += (size_t)fi;
        std::printf("%s: faces=%d\n", name.c_str(), fi);
    }

    UnionFind uf(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        for (size_t j = i + 1; j < recs.size(); ++j) {
            if (cosine(recs[i].emb, recs[j].emb) > kClusterThreshold) uf.unite((int)i, (int)j);
        }
    }
    std::map<int, std::vector<size_t>> clusters;
    for (size_t i = 0; i < recs.size(); ++i) clusters[uf.find((int)i)].push_back(i);
    std::vector<std::vector<size_t>> groups;
    for (auto& kv : clusters) groups.push_back(kv.second);
    std::sort(groups.begin(), groups.end(),
              [](const std::vector<size_t>& a, const std::vector<size_t>& b) { return a.size() > b.size(); });

    FILE* html = std::fopen((out_dir + "/index.html").c_str(), "w");
    if (!html) {
        std::fprintf(stderr, "cannot write %s/index.html\n", out_dir.c_str());
        return 1;
    }
    std::fprintf(html,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<meta charset=utf-8><title>wd-face-recognition demo</title>"
        "<style>body{font-family:system-ui;margin:12px;background:#111;color:#eee}"
        "h1{font-size:1.2em}h2{font-size:1em;margin:18px 0 6px}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(96px,1fr));gap:6px}"
        "img{width:100%%;border-radius:8px}</style>"
        "<h1>wd-face-recognition demo</h1>"
        "<p>%zu photos, %zu faces, %zu people (crude threshold clustering, no naming yet)</p>",
        photos.size(), face_total, groups.size());
    size_t gi = 0;
    for (const auto& g : groups) {
        std::fprintf(html, "<h2>Person %zu - %zu faces</h2><div class=grid>", ++gi, g.size());
        for (size_t i : g) {
            std::fprintf(html, "<img src='%s' title='%s'>", recs[i].crop.c_str(),
                         html_escape(recs[i].photo).c_str());
        }
        std::fprintf(html, "</div>");
    }
    std::fclose(html);
    std::printf("faces=%zu people=%zu -> %s/index.html\n", face_total, groups.size(), out_dir.c_str());
    return 0;
}
