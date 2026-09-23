#include "platform.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

fs::path u8path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string path_u8(const fs::path& p) {
    auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

#ifdef _WIN32
static fs::path env_path(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    return v ? fs::path(v) : fs::path();
}
#else
static fs::path env_path(const char* name) {
    const char* v = std::getenv(name);
    return v ? fs::path(v) : fs::path();
}
#endif

fs::path home_dir() {
#ifdef _WIN32
    return env_path(L"USERPROFILE");
#else
    return env_path("HOME");
#endif
}

fs::path config_dir() {
#ifdef _WIN32
    fs::path base = env_path(L"APPDATA");
    if (base.empty()) base = home_dir() / "AppData" / "Roaming";
#else
    fs::path base = env_path("XDG_CONFIG_HOME");
    if (base.empty()) base = home_dir() / ".config";
#endif
    return base / "delta";
}

fs::path expand_user(const std::string& s) {
    if (s == "~") return home_dir();
    if (s.size() >= 2 && s[0] == '~' && (s[1] == '/' || s[1] == '\\')) return home_dir() / u8path(s.substr(2));
    return u8path(s);
}

bool open_with_default_app(const fs::path& p) {
#ifdef _WIN32
    auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return r > 32;
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path_u8(p) + "\" &";
    return std::system(cmd.c_str()) == 0;
#else
    std::string cmd = "xdg-open \"" + path_u8(p) + "\" >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0;
#endif
}

bool open_url(const std::string& url) {
    // Never hand arbitrary note text to the shell: only web and mail links.
    auto starts = [&](const char* p) { return url.rfind(p, 0) == 0; };
    if (!starts("http://") && !starts("https://") && !starts("mailto:")) return false;
    if (url.find_first_of("\"\r\n") != std::string::npos) return false;
#ifdef _WIN32
    std::wstring w = u8path(url).wstring();
    auto r = (INT_PTR)ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return r > 32;
#elif defined(__APPLE__)
    return std::system(("open \"" + url + "\" &").c_str()) == 0;
#else
    return std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str()) == 0;
#endif
}

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool atomic_write(const fs::path& p, const std::string& data, std::string* err) {
    fs::path tmp = p;
    tmp += ".delta-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "can't write " + path_u8(tmp); return false; }
        f.write(data.data(), (std::streamsize)data.size());
        if (!f) { if (err) *err = "write failed for " + path_u8(tmp); return false; }
    }
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err) *err = "can't replace " + path_u8(p);
        DeleteFileW(tmp.c_str());
        return false;
    }
#else
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { if (err) *err = ec.message(); fs::remove(tmp, ec); return false; }
#endif
    return true;
}

void style_native_window(GLFWwindow* w, unsigned rgb_bg, unsigned rgb_text, bool dark) {
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(w);
    BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d);
    // COLORREF is 0x00BBGGRR.
    auto cref = [](unsigned rgb) { return (COLORREF)(((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF)); };
    COLORREF cap = cref(rgb_bg), txt = cref(rgb_text), border = cref(rgb_bg);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
    DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
    DwmSetWindowAttribute(hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof border);
#else
    (void)w; (void)rgb_bg; (void)rgb_text; (void)dark;
#endif
}
