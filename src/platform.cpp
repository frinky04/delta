#include "platform.h"
#include "util.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <dwrite_1.h>
#include <shellapi.h>
#include <wininet.h>
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
    if (ec) { if (err) *err = plain_msg(ec.message()); fs::remove(tmp, ec); return false; }
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

// ---------------------------------------------------------------------------
// Installed fonts

#ifdef _WIN32
namespace {

// Releases a COM pointer when it goes.
template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// The installed fonts, per-user ones included. Asked for afresh each time
// so fonts installed while delta runs are found.
bool system_collection(Com<IDWriteFontCollection>& out) {
    static IDWriteFactory* factory = [] {
        IDWriteFactory* f = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&f))) f = nullptr;
        return f;
    }();
    return factory && SUCCEEDED(factory->GetSystemFontCollection(&out, TRUE));
}

}  // namespace
#endif

#ifdef _WIN32
// The family's face nearest `weight`, upright and normal width; `mono` (if
// asked) says whether it's monospaced.
static bool family_face(IDWriteFontFamily* fam, int weight, FontFace& out, bool* mono = nullptr) {
    Com<IDWriteFont> font;
    Com<IDWriteFontFace> face;
    if (FAILED(fam->GetFirstMatchingFont((DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) ||
        FAILED(font->CreateFontFace(&face)))
        return false;
    if (mono) {
        Com<IDWriteFont1> font1;  // Windows 8 and later
        *mono = SUCCEEDED(font->QueryInterface(__uuidof(IDWriteFont1), (void**)&font1)) && font1->IsMonospacedFont();
    }

    // The file behind the face, if it's a file on this machine.
    UINT32 files = 0;
    if (FAILED(face->GetFiles(&files, nullptr)) || files != 1) return false;
    Com<IDWriteFontFile> file;
    Com<IDWriteFontFileLoader> loader;
    Com<IDWriteLocalFontFileLoader> local;
    const void* key = nullptr;
    UINT32 key_size = 0, len = 0;
    if (FAILED(face->GetFiles(&files, &file)) || FAILED(file->GetReferenceKey(&key, &key_size)) ||
        FAILED(file->GetLoader(&loader)) ||
        FAILED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader), (void**)&local)) ||
        FAILED(local->GetFilePathLengthFromKey(key, key_size, &len)))
        return false;
    std::wstring path(len + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, key_size, path.data(), len + 1))) return false;
    path.resize(len);

    out.file = fs::path(path);
    out.index = (int)face->GetIndex();
    // A bold DirectWrite would fake by thickening isn't in the file.
    out.weight = (font->GetSimulations() & DWRITE_FONT_SIMULATIONS_BOLD) ? 400 : (int)font->GetWeight();
    return true;
}
#endif

bool find_system_font(const std::string& family, int weight, FontFace& out) {
#ifdef _WIN32
    Com<IDWriteFontCollection> fonts;
    Com<IDWriteFontFamily> fam;
    UINT32 at = 0;
    BOOL found = FALSE;
    if (!system_collection(fonts) || FAILED(fonts->FindFamilyName(widen(family).c_str(), &at, &found)) || !found ||
        FAILED(fonts->GetFontFamily(at, &fam)))
        return false;
    return family_face(fam.p, weight, out);
#else
    (void)family; (void)weight; (void)out;
    return false;
#endif
}

std::vector<FontFamily> system_font_families() {
    std::vector<FontFamily> out;
#ifdef _WIN32
    Com<IDWriteFontCollection> fonts;
    if (!system_collection(fonts)) return out;
    for (UINT32 i = 0, n = fonts->GetFontFamilyCount(); i < n; i++) {
        Com<IDWriteFontFamily> fam;
        Com<IDWriteLocalizedStrings> names;
        if (FAILED(fonts->GetFontFamily(i, &fam)) || FAILED(fam->GetFamilyNames(&names))) continue;
        UINT32 at = 0, len = 0;
        BOOL has = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &at, &has)) || !has) at = 0;
        if (FAILED(names->GetStringLength(at, &len))) continue;
        std::wstring w(len + 1, L'\0');
        if (FAILED(names->GetString(at, w.data(), len + 1))) continue;
        w.resize(len);
        FontFamily f;
        f.name = narrow(w);
        family_face(fam.p, 400, f.regular, &f.mono);
        family_face(fam.p, 600, f.bold);
        out.push_back(std::move(f));
    }
#endif
    return out;
}

// ---------------------------------------------------------------------------
// Clipboard

#ifdef _WIN32
static UINT png_format() {
    static UINT f = RegisterClipboardFormatW(L"PNG");
    return f;
}

static bool clipboard_bytes(UINT format, std::string& out) {
    if (!IsClipboardFormatAvailable(format) || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE h = GetClipboardData(format)) {
        if (const void* p = GlobalLock(h)) {
            out.assign((const char*)p, GlobalSize(h));
            GlobalUnlock(h);
            ok = !out.empty();
        }
    }
    CloseClipboard();
    return ok;
}

static HGLOBAL global_copy(const void* data, size_t size) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!g) return nullptr;
    if (void* p = GlobalLock(g)) {
        std::memcpy(p, data, size);
        GlobalUnlock(g);
        return g;
    }
    GlobalFree(g);
    return nullptr;
}
#endif

std::vector<fs::path> clipboard_files() {
    std::vector<fs::path> out;
#ifdef _WIN32
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = (HDROP)h;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring w(len + 1, L'\0');
            DragQueryFileW(drop, i, w.data(), len + 1);
            w.resize(len);
            out.push_back(fs::path(w));
        }
    }
    CloseClipboard();
#endif
    return out;
}

bool clipboard_png(std::string& png) {
#ifdef _WIN32
    return clipboard_bytes(png_format(), png);
#else
    (void)png;
    return false;
#endif
}

bool clipboard_dib(std::string& dib) {
#ifdef _WIN32
    // CF_DIBV5 keeps alpha where the source had it; CF_DIB is the fallback
    // everyone offers (Windows makes one from the other).
    return clipboard_bytes(CF_DIBV5, dib) || clipboard_bytes(CF_DIB, dib);
#else
    (void)dib;
    return false;
#endif
}

bool clipboard_put_image(GLFWwindow* w, const std::string& png, const std::string& dib, const std::string& text) {
#ifdef _WIN32
    // An owner window is needed for SetClipboardData to take.
    if (!OpenClipboard(glfwGetWin32Window(w))) return false;
    EmptyClipboard();
    bool ok = false;
    if (!png.empty())
        if (HGLOBAL g = global_copy(png.data(), png.size())) ok |= SetClipboardData(png_format(), g) != nullptr;
    if (!dib.empty())
        if (HGLOBAL g = global_copy(dib.data(), dib.size())) ok |= SetClipboardData(CF_DIB, g) != nullptr;
    if (!text.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring wt(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wt.data(), n);
        if (HGLOBAL g = global_copy(wt.c_str(), (wt.size() + 1) * sizeof(wchar_t))) ok |= SetClipboardData(CF_UNICODETEXT, g) != nullptr;
    }
    CloseClipboard();
    return ok;
#else
    (void)w; (void)png; (void)dib; (void)text;
    return false;
#endif
}

// ---------------------------------------------------------------------------
// HTTP

bool http_get(const std::string& url, size_t max_bytes, int timeout_ms, const std::string& want_type,
              std::string& body, std::string& content_type, std::string& err) {
    body.clear();
    content_type.clear();
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
        return w;
    };
    HINTERNET net = InternetOpenW(L"delta", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) { err = "no internet connection"; return false; }
    DWORD t = (DWORD)timeout_ms;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof t);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
    InternetSetOptionW(net, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof t);
    ULONGLONG start = GetTickCount64();
    HINTERNET req = InternetOpenUrlW(net, widen(url).c_str(), nullptr, 0,
                                     INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD, 0);
    bool ok = false;
    if (!req) {
        err = "couldn't reach it";
    } else {
        DWORD status = 0, len = sizeof status;
        HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, nullptr);
        wchar_t type[256] = {};
        DWORD tlen = sizeof type;
        if (HttpQueryInfoW(req, HTTP_QUERY_CONTENT_TYPE, type, &tlen, nullptr)) {
            int n = WideCharToMultiByte(CP_UTF8, 0, type, -1, nullptr, 0, nullptr, nullptr);
            content_type.assign(n > 0 ? n - 1 : 0, '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, type, -1, content_type.data(), n, nullptr, nullptr);
        }
        std::string lowered = content_type;
        for (char& c : lowered) c = (char)std::tolower((unsigned char)c);
        if (status < 200 || status >= 300) {
            err = "the server said " + std::to_string(status);
        } else if (!want_type.empty() && lowered.rfind(want_type, 0) != 0) {
            err = "not " + want_type;
        } else {
            char buf[64 * 1024];
            DWORD got = 0;
            ok = true;
            while (InternetReadFile(req, buf, sizeof buf, &got) && got > 0) {
                body.append(buf, got);
                if (body.size() > max_bytes) { err = "too big"; ok = false; break; }
                if (GetTickCount64() - start > (ULONGLONG)timeout_ms) { err = "took too long"; ok = false; break; }
            }
            if (ok && body.empty()) { err = "it was empty"; ok = false; }
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(net);
    if (!ok) body.clear();
    return ok;
#else
    (void)url; (void)max_bytes; (void)timeout_ms; (void)want_type;
    err = "not supported here";
    return false;
#endif
}
