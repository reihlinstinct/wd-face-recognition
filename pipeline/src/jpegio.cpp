#include "jpegio.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <jpeglib.h>

namespace wfp {

namespace {

// libjpeg's default error_exit calls exit(): one corrupt or CMYK JPEG would
// kill the whole scan. Recover with setjmp/longjmp and skip the file.
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

}  // namespace

RgbImage decode_jpeg(const std::string& path, std::string* reason,
                     int64_t max_pixels) {
    if (reason) reason->clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (reason) *reason = "decode";
        return {};
    }
    jpeg_decompress_struct cinfo{};
    JpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_fatal_recover;
    // Only POD locals may live across this boundary: longjmp bypasses C++
    // destructors, so the pixel buffer is a POD new[] pointer here, adopted into the
    // RgbImage (no copy) only after jpeg_destroy_decompress has run. Locals
    // modified after setjmp are volatile so their values survive the jump.
    unsigned char* volatile buf = nullptr;
    int w = 0, h = 0;
    bool volatile created = false;
    if (setjmp(jerr.jb)) {
        std::fprintf(stderr, "jpeg decode error in %s: %s\n", path.c_str(), jerr.msg);
        if (created) jpeg_destroy_decompress(&cinfo);
        delete[] buf;
        std::fclose(f);
        if (reason) *reason = "decode";
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
        // Pixel cap BEFORE allocating (independent review #45).
        if ((int64_t)w * h > max_pixels) {
            jpeg_destroy_decompress(&cinfo);
            created = false;
            std::fclose(f);
            if (reason) *reason = "too large";
            std::fprintf(stderr, "skip oversized %s (%dx%d > %lld px)\n",
                         path.c_str(), w, h, (long long)max_pixels);
            return {};
        }
        const size_t buf_size = (size_t)w * h * 3;
        // new[] so the success path can adopt the buffer with no copy.
        buf = new (std::nothrow) unsigned char[buf_size];
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
        img.pixels.adopt(buf, (size_t)w * h * 3);  // no copy
    } else if (reason && reason->empty()) {
        *reason = "decode";
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

}  // namespace wfp
