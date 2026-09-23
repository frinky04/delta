#include "images.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8  // paths are UTF-8
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

std::string sha256_hex(const std::string& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    std::string msg = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56) msg += (char)0;
    for (int i = 7; i >= 0; i--) msg += (char)((bits >> (i * 8)) & 0xff);

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const unsigned char* p = (const unsigned char*)msg.data() + off + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    char out[65];
    for (int i = 0; i < 8; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
    return std::string(out, 64);
}

// ---------------------------------------------------------------------------
// Store

std::string ImageStore::add(const std::string& bytes, const std::string& ext, std::string* err) const {
    if (root_.empty()) { if (err) *err = "no notes folder"; return {}; }
    std::error_code ec;
    fs::path d = dir();
    bool fresh = !fs::exists(root_ / ".delta", ec);
    fs::create_directories(d, ec);
    if (ec) { if (err) *err = ec.message(); return {}; }
#ifdef _WIN32
    // Out of sight in Explorer too (dot folders already are elsewhere).
    if (fresh) {
        std::wstring w = (root_ / ".delta").wstring();
        DWORD attrs = GetFileAttributesW(w.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) SetFileAttributesW(w.c_str(), attrs | FILE_ATTRIBUTE_HIDDEN);
    }
#else
    (void)fresh;
#endif
    // 128 bits of the hash is plenty to never collide, and keeps names short.
    std::string name = sha256_hex(bytes).substr(0, 32) + "." + lower(ext);
    fs::path file = d / u8path(name);
    if (!fs::exists(file, ec)) {
        std::string e;
        if (!atomic_write(file, bytes, &e)) { if (err) *err = e; return {}; }
    }
    return link_for(name);
}

// ---------------------------------------------------------------------------
// Reading

fs::path resolve_image(const fs::path& note_dir, const std::string& src) {
    if (src.find("://") != std::string::npos || src.rfind("data:", 0) == 0) return {};
    // Links may be percent-encoded (spaces as %20).
    std::string path;
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '%' && i + 2 < src.size() && std::isxdigit((unsigned char)src[i + 1]) && std::isxdigit((unsigned char)src[i + 2])) {
            path += (char)std::strtol(src.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            path += src[i];
        }
    }
    fs::path p = u8path(path);
    if (p.is_relative()) p = note_dir / p;
    return p.lexically_normal();
}

bool image_info(const fs::path& path, int& w, int& h) {
    int comp = 0;
    return stbi_info(path_u8(path).c_str(), &w, &h, &comp) && w > 0 && h > 0;
}

void resample_rgba(const uint8_t* in, int w, int h, int tw, int th, std::vector<uint8_t>& out) {
    out.assign((size_t)tw * th * 4, 0);
    // Each target pixel averages the source area it covers, weighting the
    // partly covered edge pixels. Colour is weighted by alpha so transparent
    // pixels don't bleed their (meaningless) colour into the edges.
    double sx = (double)w / tw, sy = (double)h / th;
    std::vector<double> acc((size_t)tw * 4);
    for (int ty = 0; ty < th; ty++) {
        std::fill(acc.begin(), acc.end(), 0.0);
        double y0 = ty * sy, y1 = y0 + sy;
        double wsum_y = 0;
        for (int y = (int)y0; y < (int)std::ceil(y1) && y < h; y++) {
            double wy = std::min(y1, (double)y + 1) - std::max(y0, (double)y);
            if (wy <= 0) continue;
            wsum_y += wy;
            const uint8_t* row = in + (size_t)y * w * 4;
            for (int tx = 0; tx < tw; tx++) {
                double x0 = tx * sx, x1 = x0 + sx;
                double* a = &acc[(size_t)tx * 4];
                for (int x = (int)x0; x < (int)std::ceil(x1) && x < w; x++) {
                    double wx = std::min(x1, (double)x + 1) - std::max(x0, (double)x);
                    if (wx <= 0) continue;
                    const uint8_t* px = row + (size_t)x * 4;
                    double wt = wx * wy, al = px[3] * wt;
                    a[0] += px[0] * al;
                    a[1] += px[1] * al;
                    a[2] += px[2] * al;
                    a[3] += al;
                }
            }
        }
        double area = sx * wsum_y;
        for (int tx = 0; tx < tw; tx++) {
            double* a = &acc[(size_t)tx * 4];
            uint8_t* o = &out[((size_t)ty * tw + tx) * 4];
            if (a[3] > 0) {
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)std::clamp(a[c] / a[3] + 0.5, 0.0, 255.0);
            }
            o[3] = (uint8_t)std::clamp(a[3] / area + 0.5, 0.0, 255.0);
        }
    }
}

bool image_info_bytes(const std::string& bytes, int& w, int& h) {
    int comp = 0;
    return stbi_info_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &comp) && w > 0 && h > 0;
}

bool image_decode_bytes(const std::string& bytes, std::vector<uint8_t>& rgba, int& w, int& h) {
    int comp = 0;
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
    if (!px) return false;
    rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

std::string image_ext(const std::string& b) {
    int w, h;
    if (!image_info_bytes(b, w, h)) return {};
    if (b.compare(0, 4, "\x89PNG") == 0) return "png";
    if (b.size() > 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xD8) return "jpg";
    if (b.compare(0, 4, "GIF8") == 0) return "gif";
    if (b.compare(0, 2, "BM") == 0) return "bmp";
    return {};  // readable, but not a format worth keeping as is (TGA, PSD...)
}

std::string encode_png(const std::vector<uint8_t>& rgba, int w, int h) {
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); },
                           &out, w, h, 4, rgba.data(), w * 4);
    return out;
}

static uint32_t rd32(const std::string& s, size_t at) {
    return at + 4 <= s.size() ? (uint32_t)(uint8_t)s[at] | (uint32_t)(uint8_t)s[at + 1] << 8 | (uint32_t)(uint8_t)s[at + 2] << 16 |
                                    (uint32_t)(uint8_t)s[at + 3] << 24
                              : 0;
}
static uint16_t rd16(const std::string& s, size_t at) {
    return at + 2 <= s.size() ? (uint16_t)((uint8_t)s[at] | (uint8_t)s[at + 1] << 8) : 0;
}

bool dib_to_rgba(const std::string& d, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (d.size() < 40) return false;
    uint32_t hdr = rd32(d, 0);
    w = (int32_t)rd32(d, 4);
    int32_t sh = (int32_t)rd32(d, 8);
    uint16_t bpp = rd16(d, 14);
    uint32_t comp = rd32(d, 16);
    const uint32_t BI_RGB_ = 0, BI_BITFIELDS_ = 3;
    if (hdr < 40 || w <= 0 || sh == 0 || (bpp != 24 && bpp != 32) || (comp != BI_RGB_ && comp != BI_BITFIELDS_)) return false;
    bool top_down = sh < 0;
    h = top_down ? -sh : sh;
    // Channel masks: in the header for V4/V5, after it for a plain header.
    uint32_t rm = 0x00FF0000, gm = 0x0000FF00, bm = 0x000000FF, am = 0;
    size_t off = hdr;
    if (comp == BI_BITFIELDS_) {
        size_t at = hdr >= 56 ? 40 : hdr;
        rm = rd32(d, at);
        gm = rd32(d, at + 4);
        bm = rd32(d, at + 8);
        if (hdr >= 56) am = rd32(d, 52);
        if (hdr == 40) off += 12;
    } else if (bpp == 32) {
        am = 0xFF000000;
    }
    size_t stride = ((size_t)w * bpp + 31) / 32 * 4;
    if (off + stride * h > d.size()) return false;
    auto shift_of = [](uint32_t m) { int s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; };
    int rs = shift_of(rm), gs = shift_of(gm), bs = shift_of(bm), as = shift_of(am);
    rgba.assign((size_t)w * h * 4, 255);
    bool any_alpha = false;
    for (int y = 0; y < h; y++) {
        const uint8_t* row = (const uint8_t*)d.data() + off + stride * (top_down ? y : h - 1 - y);
        uint8_t* o = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++, o += 4) {
            if (bpp == 24) {
                o[0] = row[x * 3 + 2];
                o[1] = row[x * 3 + 1];
                o[2] = row[x * 3];
            } else {
                uint32_t v = (uint32_t)row[x * 4] | (uint32_t)row[x * 4 + 1] << 8 | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 3] << 24;
                o[0] = (uint8_t)((v & rm) >> rs);
                o[1] = (uint8_t)((v & gm) >> gs);
                o[2] = (uint8_t)((v & bm) >> bs);
                o[3] = am ? (uint8_t)((v & am) >> as) : 255;
                if (o[3]) any_alpha = true;
            }
        }
    }
    // Plenty of apps leave the fourth byte at zero: that's opaque, not invisible.
    if (bpp == 32 && !any_alpha)
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return true;
}

std::string rgba_to_dib(const std::vector<uint8_t>& rgba, int w, int h) {
    std::string d(40, '\0');
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) d[at + i] = (char)((v >> (i * 8)) & 0xFF); };
    put32(0, 40);
    put32(4, (uint32_t)w);
    put32(8, (uint32_t)h);  // bottom-up
    d[12] = 1;              // planes
    d[14] = 32;             // bits per pixel
    put32(20, (uint32_t)((size_t)w * h * 4));
    d.reserve(40 + (size_t)w * h * 4);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t* row = &rgba[(size_t)y * w * 4];
        for (int x = 0; x < w; x++) {
            const uint8_t* p = row + x * 4;
            d += (char)p[2];
            d += (char)p[1];
            d += (char)p[0];
            d += (char)p[3];
        }
    }
    return d;
}

bool image_decode(const fs::path& path, int max_w, std::vector<uint8_t>& rgba, int& w, int& h) {
    int comp = 0;
    unsigned char* px = stbi_load(path_u8(path).c_str(), &w, &h, &comp, 4);
    if (!px) return false;
    if (max_w > 0 && w > max_w) {
        int tw = max_w, th = std::max(1, (int)std::lround((double)h * tw / w));
        resample_rgba(px, w, h, tw, th, rgba);
        w = tw;
        h = th;
    } else {
        rgba.assign(px, px + (size_t)w * h * 4);
    }
    stbi_image_free(px);
    return true;
}
