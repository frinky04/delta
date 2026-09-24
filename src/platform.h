#pragma once
// OS-specific bits: paths, opening things with the default app, atomic
// writes, native window styling.
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
struct GLFWwindow;

fs::path u8path(const std::string& s);
std::string path_u8(const fs::path& p);

fs::path home_dir();
fs::path config_dir();                 // %APPDATA%/delta or ~/.config/delta
fs::path expand_user(const std::string& s);  // "~/x" -> home/x

bool open_with_default_app(const fs::path& p);
bool open_url(const std::string& url); // http(s) and mailto only

bool read_file(const fs::path& p, std::string& out);
bool atomic_write(const fs::path& p, const std::string& data, std::string* err);

// Dark title bar + caption colour on Windows 11; no-op elsewhere.
void style_native_window(GLFWwindow* w, unsigned rgb_bg, unsigned rgb_text, bool dark);

// An installed font face: its file, and which face it is when the file holds
// several (.ttc).
struct FontFace {
    fs::path file;
    int index = 0;
    int weight = 400;  // as found, which is the nearest to the one asked for
};
// The installed face of `family` nearest `weight` (400 regular, 600
// semibold), upright and normal width. False if there's no such family, or
// it isn't a local file. Windows (DirectWrite) only; elsewhere finds nothing.
bool find_system_font(const std::string& family, int weight, FontFace& out);
// Every installed family's name, for suggesting one.
std::vector<std::string> system_font_families();

// The clipboard beyond text (Windows; elsewhere these find nothing).
std::vector<fs::path> clipboard_files();      // files copied in a file manager
bool clipboard_png(std::string& png);          // an image offered as PNG bytes
bool clipboard_dib(std::string& dib);          // an image offered as a bitmap (BITMAPINFO + pixels)
// Puts an image on the clipboard, as PNG and as a bitmap for apps that only
// take that, together with text.
bool clipboard_put_image(GLFWwindow* w, const std::string& png, const std::string& dib, const std::string& text);

// Fetches an http(s) URL. Gives up past `max_bytes` or `timeout_ms`, and as
// soon as the reply's type doesn't start with `want_type` (if given).
bool http_get(const std::string& url, size_t max_bytes, int timeout_ms, const std::string& want_type,
              std::string& body, std::string& content_type, std::string& err);
