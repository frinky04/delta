#pragma once
// OS-specific bits: paths, opening things with the default app, atomic
// writes, native window styling.
#include <filesystem>
#include <string>

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
