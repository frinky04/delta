#pragma once
// Images in notes. They live out of sight in one store for the whole notes
// folder, <notes>/.delta/images, named by a hash of their contents: the same
// image is stored once, and renaming or moving notes never touches them.
// Notes refer to them with ordinary markdown, ![](../.delta/images/<hash>.png)
// (spaces are one level deep, so that path works from any space), which
// other markdown tools show too.
#include <cstdint>
#include <string>
#include <vector>

#include "platform.h"

// Hex SHA-256 of some bytes.
std::string sha256_hex(const std::string& data);

class ImageStore {
public:
    void set_root(const fs::path& notes_root) { root_ = notes_root; }
    fs::path dir() const { return root_ / ".delta" / "images"; }

    // Stores the bytes of an image (ext without the dot: "png", "jpg") and
    // returns the link a note uses for it, or "" with `err` set.
    std::string add(const std::string& bytes, const std::string& ext, std::string* err = nullptr) const;
    // The link for a file already in the store.
    static std::string link_for(const std::string& file_name) { return "../.delta/images/" + file_name; }

private:
    fs::path root_;
};

// Where an image a note refers to as `src` is on disk, from the note's own
// folder. Empty for web addresses: images are only ever read from disk.
fs::path resolve_image(const fs::path& note_dir, const std::string& src);

// Pixel size from the file's header, without decoding it.
bool image_info(const fs::path& path, int& w, int& h);

// Decodes an image to RGBA, scaled down (averaging, so fine detail like
// text in screenshots stays clean) to at most `max_w` pixels wide.
bool image_decode(const fs::path& path, int max_w, std::vector<uint8_t>& rgba, int& w, int& h);

// Area-averaging downscale of RGBA pixels to tw x th (no larger than the source).
void resample_rgba(const uint8_t* in, int w, int h, int tw, int th, std::vector<uint8_t>& out);

// Bytes of an image file in memory: its size, its pixels, and the file
// extension its format goes by ("png", "jpg", "gif", "bmp"; "" if it isn't a
// format delta can show).
bool image_info_bytes(const std::string& bytes, int& w, int& h);
bool image_decode_bytes(const std::string& bytes, std::vector<uint8_t>& rgba, int& w, int& h);
std::string image_ext(const std::string& bytes);

// RGBA pixels as a PNG file.
std::string encode_png(const std::vector<uint8_t>& rgba, int w, int h);

// Windows clipboard bitmaps (BITMAPINFOHEADER or V5 header, then pixels).
bool dib_to_rgba(const std::string& dib, std::vector<uint8_t>& rgba, int& w, int& h);
std::string rgba_to_dib(const std::vector<uint8_t>& rgba, int w, int h);
