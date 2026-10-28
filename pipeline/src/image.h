// Shared decoded-image type for the face pipeline (issue #40, PR 2).
// PixelBuf is unique_ptr-backed so JPEG decode can hand its buffer over
// WITHOUT a second full-image copy (independent review #45): the old
// vector.assign() doubled peak RAM on big photos. Move-only by design.
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>

namespace wfp {

struct PixelBuf {
    std::unique_ptr<unsigned char[]> p;
    size_t n = 0;
    unsigned char* data() { return p.get(); }
    const unsigned char* data() const { return p.get(); }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    unsigned char& operator[](size_t i) { return p[i]; }
    const unsigned char& operator[](size_t i) const { return p[i]; }
    // Raw allocation, no zero-init: callers (chips, decode) write every byte.
    void resize(size_t m) { p.reset(new unsigned char[m]); n = m; }
    void assign(size_t m, unsigned char v) { resize(m); std::memset(p.get(), v, m); }
    void assign(const unsigned char* b, const unsigned char* e) {
        resize((size_t)(e - b));
        std::memcpy(p.get(), b, e - b);
    }
    // Takes ownership of a new[]'d buffer without copying.
    void adopt(unsigned char* buf, size_t m) { p.reset(buf); n = m; }
};

struct RgbImage {
    int w = 0, h = 0;
    PixelBuf pixels;  // w*h*3, row-major RGB
};

}  // namespace wfp
