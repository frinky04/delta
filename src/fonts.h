#pragma once
// Which fonts the text is drawn in: the embedded Noto faces, or ones the
// config names ([editor] font and mono_font).
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

// The faces one font setting picks. An empty file is the embedded font.
struct FontPick {
    FontFace regular;
    FontFace bold;  // semibold or heavier; no file: none, so bold is drawn doubled
    bool embedded() const { return regular.file.empty(); }
};

// A font setting that's a path rather than a family name: it has a folder in
// it, or a font file's extension.
bool is_font_path(const std::string& value);

// Is `bold` a heavier face of its own? Not when it's the regular face again:
// a family without heavier weights, or a variable font, whose other weights
// ImGui can't draw.
bool real_bold(const FontFace& regular, const FontFace& bold);

// Reads a font setting: empty or `builtin` (the embedded font's name), an
// installed family ("JetBrains Mono"), or the path of a font file. Returns
// what's wrong ("" if nothing), and then `out` is the embedded font.
std::string pick_font(const std::string& value, const std::string& builtin, FontPick& out);

// A font file's bytes, shared by every font made from it.
using FontBytes = std::shared_ptr<const std::string>;

// A font setting found and its files read: all the UI thread needs to add
// it to ImGui without waiting on DirectWrite or the disk.
struct FontLoad {
    std::string value;    // the setting, as asked for
    std::string problem;  // pick_font's
    FontPick pick;
    FontBytes regular;    // null: the embedded font
    FontBytes bold;       // null: none (the embedded font's, if regular is)
};

// The slow part of fonts, on a worker: finding them and reading their files
// (tens of milliseconds each, far more with thousands installed or a cold
// disk). A file is read once while anything still holds its bytes. The
// newest request of each kind wins, and take_* hands its result over once.
class FontLoader {
public:
    struct Settings {
        std::string sans, sans_builtin, mono, mono_builtin;  // values, and their embedded fonts' names
    };
    struct Pair {
        FontLoad sans, mono;
    };

    FontLoader();
    ~FontLoader();
    FontLoader(const FontLoader&) = delete;
    FontLoader& operator=(const FontLoader&) = delete;

    // Both font settings, now: for start-up, when there's nothing to draw
    // with until they're in.
    Pair load_now(const Settings& s);
    // The same on the worker.
    void load(const Settings& s);
    bool take(Pair& out);
    // One setting, for the font picker's preview.
    void preview(const std::string& value, const std::string& builtin);
    bool take_preview(FontLoad& out);
    // Every installed family, sorted by name.
    void list_families();
    bool take_families(std::vector<FontFamily>& out);
    // Whether anything asked for isn't in yet (worth another frame).
    bool busy();

    // The system's symbol fonts, for glyphs Noto lacks (arrows, emoji). Read
    // the first time they're asked for.
    const std::vector<FontBytes>& symbols();

private:
    FontLoad resolve(const std::string& value, const std::string& builtin);
    FontBytes read(const fs::path& p);
    void run();

    std::mutex cache_mu_;  // cache_, symbols_
    std::map<fs::path, std::weak_ptr<const std::string>> cache_;
    std::vector<FontBytes> symbols_;
    bool symbols_read_ = false;

    std::mutex mu_;  // everything below
    std::condition_variable cv_;
    std::optional<Settings> load_want_;
    std::optional<Pair> load_done_;
    std::optional<std::pair<std::string, std::string>> preview_want_;  // value, builtin
    std::optional<FontLoad> preview_done_;
    bool families_want_ = false;
    std::optional<std::vector<FontFamily>> families_done_;
    bool working_ = false, stop_ = false;
    std::thread thread_;
};
