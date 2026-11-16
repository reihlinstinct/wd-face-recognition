// wdf-bench-cpp: language shootout, C++ side (issue #14).
// Subcommands: sha256 | exif | jpeg | http-server | http-client | sqlite
// Each workload prints one line: "ops_per_sec=<double>" (or for http-server
// nothing; the client measures). Peak RSS is captured by the runner.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <jpeglib.h>
#include <exiv2/exiv2.hpp>
#include <sqlite3.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {

constexpr size_t kStreamChunk = 1 << 16;

int workload_sha256(const char* path, int iterations) {
    const auto t0 = std::chrono::steady_clock::now();
    unsigned long long total_bytes = 0;
    for (int it = 0; it < iterations; ++it) {
        std::ifstream in(path, std::ios::binary);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
        std::vector<char> buf(kStreamChunk);
        while (in) {
            in.read(buf.data(), buf.size());
            const std::streamsize n = in.gcount();
            if (n > 0) {
                EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n));
                total_bytes += static_cast<unsigned long long>(n);
            }
        }
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len = 0;
        EVP_DigestFinal_ex(ctx, digest, &digest_len);
        EVP_MD_CTX_free(ctx);
        if (it == 0) std::printf("digest0=%02x%02x%02x%02x\n", digest[0], digest[1], digest[2], digest[3]);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("mb_per_sec=%.2f\n", total_bytes / 1e6 / secs);
    return 0;
}

int workload_exif(const char* path, int iterations) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int it = 0; it < iterations; ++it) {
        auto image = Exiv2::ImageFactory::open(path);
        image->readMetadata();
        const Exiv2::ExifData& data = image->exifData();
        if (it == 0) {
            auto make = data.findKey(Exiv2::ExifKey("Exif.Image.Make"));
            if (make != data.end()) std::printf("make=%s\n", make->toString().c_str());
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("ops_per_sec=%.2f\n", iterations / secs);
    return 0;
}

int workload_jpeg(const char* path, int iterations) {
    const auto t0 = std::chrono::steady_clock::now();
    unsigned long long pixels = 0;
    for (int it = 0; it < iterations; ++it) {
        FILE* f = std::fopen(path, "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
        jpeg_decompress_struct cinfo{};
        jpeg_error_mgr jerr{};
        cinfo.err = jpeg_std_error(&jerr);
        jpeg_create_decompress(&cinfo);
        jpeg_stdio_src(&cinfo, f);
        jpeg_read_header(&cinfo, TRUE);
        cinfo.out_color_space = JCS_RGB;
        jpeg_start_decompress(&cinfo);
        std::vector<unsigned char> row(cinfo.output_width * 3);
        while (cinfo.output_scanline < cinfo.output_height) {
            unsigned char* ptr = row.data();
            jpeg_read_scanlines(&cinfo, &ptr, 1);
        }
        pixels += static_cast<unsigned long long>(cinfo.output_width) * cinfo.output_height;
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        std::fclose(f);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("photos_per_sec=%.3f mpix_per_sec=%.2f\n", iterations / secs, pixels / 1e6 / secs);
    return 0;
}

const char* kWorkResponse = R"({"ok":true,"faces":3,"elapsed_ms":42})";

int workload_http_server(int port) {
    httplib::Server server;
    server.Post("/work", [](const httplib::Request& req, httplib::Response& res) {
        // Realistic local-API shape: parse the JSON body, answer with JSON.
        auto body = nlohmann::json::parse(req.body, nullptr, /*throw=*/false);
        (void)body;
        res.set_content(kWorkResponse, "application/json");
    });
    server.Get("/ready", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ready", "text/plain");
    });
    std::fprintf(stderr, "listening on %d\n", port);
    return server.listen("127.0.0.1", port) ? 0 : 1;
}

int workload_http_client(int port, int requests) {
    // One connection per request: keep-alive was measured first, but without
    // TCP_NODELAY on every server (tiny_http cannot set it) delayed-ACK
    // stalls collapsed throughput to ~20 req/s for all servers. Fresh
    // connections treat every server identically and keep server cost
    // dominant.
    httplib::Client client("127.0.0.1", port);
    const std::string payload = R"({"photo":"photo_12mp.jpg","min_confidence":0.65})";
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < requests; ++i) {
        auto res = client.Post("/work", payload, "application/json");
        if (res && res->status == 200) ++ok;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("req_per_sec=%.2f ok=%d\n", requests / secs, ok);
    return ok == requests ? 0 : 1;
}

int workload_sqlite(const char* dir, int rows) {
    const std::string db_path = std::string(dir) + "/bench_cpp.sqlite";
    std::remove(db_path.c_str());
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return 1;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE TABLE faces (id INTEGER PRIMARY KEY, photo TEXT, x REAL, y REAL, embedding BLOB);",
                 nullptr, nullptr, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO faces (photo, x, y, embedding) VALUES (?1, ?2, ?3, zeroblob(512));",
                       -1, &stmt, nullptr);
    for (int i = 0; i < rows; ++i) {
        sqlite3_bind_text(stmt, 1, "photo_12mp.jpg", -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 2, 0.1 * (i % 100));
        sqlite3_bind_double(stmt, 3, 0.2 * (i % 50));
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sqlite3_close(db);
    std::remove(db_path.c_str());
    std::printf("rows_per_sec=%.2f\n", rows / secs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <workload> [args]\n", argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    const int iterations = argc > 3 ? std::atoi(argv[3]) : 10;
    if (cmd == "sha256") return workload_sha256(argv[2], iterations);
    if (cmd == "exif") return workload_exif(argv[2], iterations);
    if (cmd == "jpeg") return workload_jpeg(argv[2], iterations);
    if (cmd == "http-server") return workload_http_server(std::atoi(argv[2]));
    if (cmd == "http-client") return workload_http_client(std::atoi(argv[2]), iterations);
    if (cmd == "sqlite") return workload_sqlite(argv[2], iterations);
    std::fprintf(stderr, "unknown workload: %s\n", cmd.c_str());
    return 2;
}
