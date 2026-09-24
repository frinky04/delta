#include "fonts.h"

#include <algorithm>
#include <fstream>
#include <vector>

#include "config.h"
#include "util.h"

bool is_font_path(const std::string& v) {
    if (v.find_first_of("/\\~") != std::string::npos) return true;
    std::string ext = lower(path_u8(u8path(v).extension()));
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

// A TrueType or OpenType font, or a collection of them: what ImGui can draw.
// (Not WOFF, or the old bitmap .fon.)
static bool is_font_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    char tag[4] = {};
    if (!in.read(tag, 4)) return false;
    std::string t(tag, 4);
    return t == std::string("\0\1\0\0", 4) || t == "OTTO" || t == "true" || t == "ttcf";
}

bool real_bold(const FontFace& regular, const FontFace& bold) {
    return !bold.file.empty() && bold.weight >= 550 && !(bold.file == regular.file && bold.index == regular.index);
}

std::string pick_font(const std::string& value, const std::string& builtin, FontPick& out) {
    out = {};
    if (value.empty() || iequals(value, builtin)) return {};
    if (is_font_path(value)) {
        fs::path p = expand_user(value);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return "no font file here, using " + builtin;
        if (!is_font_file(p)) return "not a font delta can read (.ttf, .otf or .ttc), using " + builtin;
        out.regular.file = p;  // one file is one weight: bold is drawn doubled
        return {};
    }

    FontFace regular;
    if (!find_system_font(value, 400, regular)) {
        std::vector<FontFamily> families = system_font_families();
        if (families.empty()) return "can't look up installed fonts here; give the font file's path; using " + builtin;
        std::vector<std::string> lowered;
        for (auto& f : families) lowered.push_back(lower(f.name));
        std::string near = suggest(lower(value), lowered);
        // Installed, but not as a file here (a cloud font not yet downloaded).
        if (near == lower(value)) return "this font isn't on this computer as a file, using " + builtin;
        for (auto& f : families)
            if (!near.empty() && lower(f.name) == near) return "no installed font with this name; did you mean " + f.name + "?";
        return "no installed font with this name, using " + builtin;
    }
    if (!is_font_file(regular.file)) return "this font's file isn't one delta can read, using " + builtin;
    out.regular = regular;
    FontFace bold;
    if (find_system_font(value, 600, bold) && real_bold(regular, bold) && is_font_file(bold.file)) out.bold = bold;
    return {};
}

// ---------------------------------------------------------------------------
// FontLoader

FontLoader::FontLoader() : thread_([this] { run(); }) {}

FontLoader::~FontLoader() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

FontBytes FontLoader::read(const fs::path& p) {
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        if (auto it = cache_.find(p); it != cache_.end())
            if (FontBytes held = it->second.lock()) return held;
    }
    std::string bytes;
    if (!read_file(p, bytes) || bytes.empty()) return nullptr;
    FontBytes b = std::make_shared<const std::string>(std::move(bytes));
    std::lock_guard<std::mutex> lock(cache_mu_);
    // Nothing holds them any more: the bytes are gone, so their entries go too.
    for (auto it = cache_.begin(); it != cache_.end();) it = it->second.expired() ? cache_.erase(it) : std::next(it);
    cache_[p] = b;
    return b;
}

FontLoad FontLoader::resolve(const std::string& value, const std::string& builtin) {
    FontLoad l;
    l.value = value;
    l.problem = pick_font(value, builtin, l.pick);
    if (l.pick.embedded()) return l;
    l.regular = read(l.pick.regular.file);
    if (!l.regular) {
        l.problem = "couldn't read this font's file, using " + builtin;
        l.pick = {};
        return l;
    }
    if (!l.pick.bold.file.empty()) l.bold = read(l.pick.bold.file);
    if (!l.bold) l.pick.bold = {};
    return l;
}

FontLoader::Pair FontLoader::load_now(const Settings& s) {
    return {resolve(s.sans, s.sans_builtin), resolve(s.mono, s.mono_builtin)};
}

void FontLoader::load(const Settings& s) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        load_want_ = s;
        load_done_.reset();  // outdated now
    }
    cv_.notify_all();
}

bool FontLoader::take(Pair& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!load_done_ || load_want_) return false;  // a newer one is coming
    out = std::move(*load_done_);
    load_done_.reset();
    return true;
}

void FontLoader::preview(const std::string& value, const std::string& builtin) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        preview_want_ = {value, builtin};
        preview_done_.reset();
    }
    cv_.notify_all();
}

bool FontLoader::take_preview(FontLoad& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!preview_done_ || preview_want_) return false;
    out = std::move(*preview_done_);
    preview_done_.reset();
    return true;
}

void FontLoader::list_families() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        families_want_ = true;
    }
    cv_.notify_all();
}

bool FontLoader::take_families(std::vector<FontFamily>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!families_done_ || families_want_) return false;
    out = std::move(*families_done_);
    families_done_.reset();
    return true;
}

bool FontLoader::busy() {
    std::lock_guard<std::mutex> lock(mu_);
    return working_ || load_want_ || preview_want_ || families_want_ || load_done_ || preview_done_ || families_done_;
}

const std::vector<FontBytes>& FontLoader::symbols() {
    std::lock_guard<std::mutex> lock(cache_mu_);
    if (!symbols_read_) {
        symbols_read_ = true;
#ifdef _WIN32
        for (const char* path : {"C:\\Windows\\Fonts\\seguisym.ttf", "C:\\Windows\\Fonts\\seguiemj.ttf"}) {
            std::string bytes;
            if (read_file(path, bytes) && !bytes.empty()) symbols_.push_back(std::make_shared<const std::string>(std::move(bytes)));
        }
#endif
    }
    return symbols_;
}

void FontLoader::run() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || load_want_ || preview_want_ || families_want_; });
        if (stop_) return;
        working_ = true;
        // The fonts in use first, then the preview, then the list.
        if (load_want_) {
            Settings s = std::move(*load_want_);
            load_want_.reset();
            lock.unlock();
            Pair p = load_now(s);
            lock.lock();
            if (!load_want_) load_done_ = std::move(p);
        } else if (preview_want_) {
            auto [value, builtin] = std::move(*preview_want_);
            preview_want_.reset();
            lock.unlock();
            FontLoad l = resolve(value, builtin);
            lock.lock();
            if (!preview_want_) preview_done_ = std::move(l);
        } else {
            families_want_ = false;
            lock.unlock();
            std::vector<FontFamily> fams = system_font_families();
            std::vector<std::pair<std::string, size_t>> keys;
            for (size_t i = 0; i < fams.size(); i++) keys.push_back({lower(fams[i].name), i});
            std::sort(keys.begin(), keys.end());
            std::vector<FontFamily> sorted;
            for (auto& k : keys) sorted.push_back(std::move(fams[k.second]));
            lock.lock();
            if (!families_want_) families_done_ = std::move(sorted);
        }
        working_ = false;
    }
}
