// wdf_demo: first visible end-to-end slice (issue #18).
// Scans a folder of JPEGs recursively (scan.h, issue #24), detects faces
// (SCRFD-500M), aligns + embeds them (MobileFaceNet ArcFace), clusters by
// cosine similarity, and writes a mobile-friendly HTML contact sheet
// grouped by person.
//
// Usage: wdf_demo <photos_dir> <models_dir> <out_dir> [threads]
//
// The pipeline pieces live in pipeline/src (shared with the indexer since
// 0.3.0 PR 2, issue #40); only the scan caps and the HTML report are
// demo-local. Clustering stays crude threshold clustering - the indexer
// owns the real people model (0.3.0 PR 3).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <net.h>

#include "align.h"
#include "cluster.h"
#include "detect.h"
#include "jpegio.h"
#include "scan.h"

namespace {

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

}  // namespace

int main(int argc, char** argv) {
    // Line-buffered stdout: the async demo runner (issue #33) redirects
    // stdout to run.log, where full buffering would hide the per-photo
    // progress lines until exit.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc >= 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("wdf_demo 0.2.5 (SCRFD-500M + MobileFaceNet, ncnn)\n");
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
    wfp::load_net(det, models_dir, "scrfd_500m", threads);
    wfp::load_net(rec, models_dir, "mobilefacenet", threads);

    mkdir(out_dir.c_str(), 0777);
    mkdir((out_dir + "/crops").c_str(), 0777);

    wdf::ScanOptions scan_opts;  // demo caps: max_depth 8, max_photos 1000
    wdf::ScanResult scan = wdf::scan_photos(photos_dir, scan_opts);
    if (scan.photos.empty()) {
        struct stat st;
        if (stat(photos_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            std::fprintf(stderr, "cannot open %s\n", photos_dir.c_str());
            return 1;
        }
    }
    const std::vector<std::string>& photos = scan.photos;
    std::printf("photos=%zu\n", photos.size());
    if (scan.truncated_photos)
        std::printf("note: scan stopped at the %zu-photo demo cap\n",
                    scan_opts.max_photos);
    if (scan.truncated_depth)
        std::printf("note: folders deeper than %d levels were skipped\n",
                    scan_opts.max_depth);
    if (scan.truncated_entries)
        std::printf("note: scan stopped after examining %zu entries (work budget)\n",
                    scan_opts.max_entries);

    struct Rec {
        std::string photo;
        std::string crop;
        std::vector<float> emb;
    };
    std::vector<Rec> recs;
    size_t face_total = 0;
    for (const std::string& name : photos) {
        wfp::RgbImage img = wfp::decode_jpeg(photos_dir + "/" + name);
        if (img.pixels.empty()) {
            std::fprintf(stderr, "skip undecodable %s\n", name.c_str());
            continue;
        }
        std::vector<wfp::Face> faces = wfp::scrfd_detect(det, img);
        int fi = 0;
        for (const wfp::Face& f : faces) {
            // Aligned chip via the SCRFD landmarks; margin-crop fallback when
            // the landmarks come back degenerate (issue #40 PR 2).
            wfp::RgbImage chip = wfp::align_chip(img, f.kps);
            if (chip.pixels.empty()) chip = wfp::crop_chip(img, f.x0, f.y0, f.x1, f.y1);
            if (chip.pixels.empty()) continue;  // degenerate box, rejected by both paths
            Rec r;
            r.photo = name;
            r.crop = "crops/f" + std::to_string(recs.size()) + ".jpg";
            r.emb = wfp::arcface_embed(rec, chip);
            if (!wfp::encode_jpeg(chip.pixels.data(), chip.w, chip.h, out_dir + "/" + r.crop)) {
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

    wfp::UnionFind uf(recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        for (size_t j = i + 1; j < recs.size(); ++j) {
            if (wfp::cosine(recs[i].emb, recs[j].emb) > kClusterThreshold) uf.unite((int)i, (int)j);
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
