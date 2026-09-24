#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstring>
#include <set>
#include <sstream>

#include "commands.h"
#include "imgui_freetype.h"
#include "problems.h"
#include "spell_fix.h"
#include "theme.h"
#include "util.h"

extern const unsigned char g_font_sans[];
extern const std::size_t g_font_sans_size;
extern const unsigned char g_font_mono[];
extern const std::size_t g_font_mono_size;
extern const unsigned char g_font_sans_bold[];
extern const std::size_t g_font_sans_bold_size;
extern const unsigned char g_font_mono_bold[];
extern const std::size_t g_font_mono_bold_size;
extern const unsigned char g_manual[];
extern const std::size_t g_manual_size;

namespace {

std::string ago(std::time_t t) {
    if (!t) return {};
    long long d = (long long)(std::time(nullptr) - t);
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + "m";
    if (d < 86400) return std::to_string(d / 3600) + "h";
    if (d < 86400 * 14) return std::to_string(d / 86400) + "d";
    if (d < 86400 * 90) return std::to_string(d / (86400 * 7)) + "w";
    char buf[32];
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

std::string quote(const std::string& s) { return "\xE2\x80\x9C" + s + "\xE2\x80\x9D"; }

fs::file_time_type mtime_of(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type{} : t;
}

}  // namespace

Place App::here() const {
    Place p;
    if (note_) p = {*note_, editor_.cursor(), editor_.scroll()};
    return p;
}

// A jump, for back and forward: where you were when it began and where it
// left you go in the history. Nested (a search result opening its note),
// only the outermost one counts.
struct App::Jump {
    App& app;
    History::Kind kind;
    explicit Jump(App& a, History::Kind k = History::Other) : app(a), kind(k) {
        if (app.jump_depth_++ == 0) app.jump_from_ = app.here();
    }
    ~Jump() {
        if (--app.jump_depth_ == 0) app.history_.jumped(app.jump_from_, app.here(), kind);
    }
};

// ---------------------------------------------------------------------------
// Setup

bool App::init(GLFWwindow* window, const AppOptions& opts) {
    win_ = window;
    sandbox_ = opts.sandbox;
    images_.start();
    cfg_path_ = opts.config_path.empty() ? config_dir() / "delta.ini" : opts.config_path;
    state_path_ = cfg_path_.parent_path() / "state.ini";
    dict_dir_ = cfg_path_.parent_path() / "dictionaries";
    words_path_ = cfg_path_.parent_path() / "words.txt";
    pending_run_ = opts.run;

    register_commands();
    load_settings();
    apply_fonts();
    font_loader_.list_families();  // ready for the font picker

    bool first_launch = !fs::exists(state_path_);  // it's written on every quit
    auto state = load_kv(state_path_);
    if (auto it = state.find("zoom"); it != state.end()) zoom_ = std::clamp((float)std::atof(it->second.c_str()), 0.5f, 3.0f);
    if (state.count("width") && state.count("height")) {
        int w = std::atoi(state["width"].c_str()), h = std::atoi(state["height"].c_str());
        if (w >= 320 && h >= 240) glfwSetWindowSize(win_, w, h);
    }
    if (state.count("x") && state.count("y")) {
        int x = std::atoi(state["x"].c_str()), y = std::atoi(state["y"].c_str());
        // Only restore if it lands on some monitor.
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        for (int i = 0; i < count; i++) {
            int mx, my, mw, mh;
            glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
            if (x >= mx - 50 && y >= my - 50 && x < mx + mw - 100 && y < my + mh - 100) { glfwSetWindowPos(win_, x, y); break; }
        }
    }
    if (state["reader"] == "1") editor_.set_reader(true, false);
    sort_recent_ = state["sort"] == "recent";
    // Carets and scroll from last time ("view:<path> = line col scroll").
    for (auto& [key, value] : state) {
        if (key.rfind("view:", 0) != 0) continue;
        int line = 0, col = 0, scroll = 0;
        if (std::sscanf(value.c_str(), "%d %d %d", &line, &col, &scroll) != 3) continue;
        views_[path_u8(store_.root() / u8path(key.substr(5)))] = {TextPos{line, col}, (float)scroll};
    }

    editor_.on_link = [this](const LinkSpan& l) { open_link(l); };

    space_ = state.count("space") ? state["space"] : cfg_.default_space;
    std::error_code ec;
    if (space_.empty() || !fs::is_directory(store_.root() / u8path(space_), ec)) space_ = cfg_.default_space;
    store_.ensure_space(space_);

    if (state.count("note") && !state["note"].empty() && store_.exists(space_, state["note"])) {
        open_note(store_.ref(space_, state["note"]));
    } else {
        auto notes = store_.notes(space_);
        if (!notes.empty()) open_note(notes.front());
    }
    if (opts.sandbox) status("sandbox: " + path_u8(cfg_path_.parent_path()));
    // The first launch opens the manual, over whatever else would open (back
    // returns to it), with the ways out in the status line.
    if (first_launch) {
        open_manual();
        std::string tip;
        for (auto [id, what] : {std::pair{"note.new", "new note"}, {"palette.notes", "find a note"}})
            if (std::string k = chord_for(id); !k.empty()) tip += (tip.empty() ? "" : " \xC2\xB7 ") + k + " " + what;
        if (!tip.empty()) status(tip);
    }
    set_title();
    return true;
}

void App::load_settings() {
    std::vector<ConfigIssue> issues;
    std::error_code ec;
    if (!fs::exists(cfg_path_, ec)) {
        fs::create_directories(cfg_path_.parent_path(), ec);
        atomic_write(cfg_path_, default_config_text(), nullptr);
    }
    Config c;
    if (!load_config(cfg_path_, c, issues)) issues.push_back({-1, "couldn't read " + path_u8(cfg_path_)});
    cfg_ = c;
    cfg_mtime_ = mtime_of(cfg_path_);

    show_theme(cfg_.theme);
    anim::enabled = cfg_.animations;
    anim::speed = cfg_.animation_speed;
    apply_bindings();
    check_settings(cfg_, issues);

    fs::path root = expand_user(cfg_.notes_dir);
    fs::create_directories(root, ec);
    if (ec) issues.push_back(issue_at(cfg_, "general.notes_dir", true, "can't create this folder: " + plain_msg(ec.message())));
    store_.set_root(root);
    image_store_.set_root(root);
    if (!Store::check_name(cfg_.default_space, "space").empty()) cfg_.default_space = "default";
    if (!Store::check_name(cfg_.daily_space, "space").empty()) cfg_.daily_space = "daily";
    apply_spelling();
    // Fonts are found and read on the font worker and loaded between frames
    // (apply_fonts), so a change shows soon after the config is saved. At
    // start-up there's nothing to draw with until they're in, so they're
    // found here.
    FontLoader::Settings fonts{cfg_.font, find_setting("editor", "font")->def, cfg_.mono_font,
                               find_setting("editor", "mono_font")->def};
    if (!sans_) fonts_in_ = font_loader_.load_now(fonts);
    else font_loader_.load(fonts);

    config_issues_ = std::move(issues);
    hints_version_ = UINT64_MAX;  // colour defaults follow the theme
    publish_problems(true);
}

// The config's problems: load_settings' own and its fonts' (which the font
// worker finds a little later). On their lines when the config is open in
// delta; otherwise, if `announce`, the first one goes to the status bar.
void App::publish_problems(bool announce) {
    std::vector<ConfigIssue> all = config_issues_;
    for (auto& [key, fp] : font_problems_) {
        // Only while the setting still says what was found wrong with it.
        const std::string& value = key == "font" ? cfg_.font : cfg_.mono_font;
        if (!fp.problem.empty() && fp.value == value) all.push_back(issue_at(cfg_, "editor." + key, true, fp.problem));
    }
    set_problems(std::move(all));
    const auto& shown = config_problems_;
    if (announce && !shown.empty() && !is_config()) {
        const ConfigIssue& first = shown[0];
        std::string msg = "config: " + (first.line >= 0 ? "line " + std::to_string(first.line + 1) + ", " : std::string()) + first.msg;
        if (shown.size() > 1) msg += " (+" + std::to_string(shown.size() - 1) + " more)";
        status(msg, true);
    }
}

// One font: the picked face (`bytes`, if any), then the embedded face for the
// glyphs it lacks, then the system's symbol fonts for the glyphs Noto lacks
// (arrows, emoji). All borrowed, never copied: whoever loads a font holds its
// bytes for as long as it's loaded (the loader holds the symbol fonts').
ImFont* App::load_font(const FontBytes& bytes, int index, const unsigned char* noto, std::size_t noto_size) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    ImFont* font = nullptr;
    auto add = [&](const void* data, std::size_t size, int face) {
        ImFontConfig c;
        c.FontDataOwnedByAtlas = false;
        c.FontNo = (ImU32)face;
        c.MergeMode = font != nullptr;
        c.DstFont = font;
        // Light hinting snaps glyphs to the pixel grid vertically only, so
        // stems and edges are crisp without bending the letter shapes.
        c.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting | ImGuiFreeTypeLoaderFlags_LoadColor;
        ImFont* f = atlas->AddFontFromMemoryTTF(const_cast<void*>(data), (int)size, 17.0f, &c);
        if (!font) font = f;
    };
    if (bytes) add(bytes->data(), bytes->size(), index);
    add(noto, noto_size, 0);
    for (auto& s : font_loader_.symbols()) add(s->data(), s->size(), 0);
    return font;
}

// Takes in what the font worker has for us, between frames: the fonts the
// config picks, the preview's, the list of installed ones.
void App::apply_fonts() {
    FontLoader::Pair in;
    if (fonts_in_) {
        in = std::move(*fonts_in_);
        fonts_in_.reset();
        install_fonts(in);
    } else if (font_loader_.take(in)) {
        install_fonts(in);
    }

    if (preview_want_ != preview_asked_) {
        preview_asked_ = preview_want_;
        if (!preview_want_.empty()) {
            bool mono = preview_want_[0] == 'm';
            font_loader_.preview(preview_want_.substr(2), find_setting("editor", mono ? "mono_font" : "font")->def);
        }
    }
    // Nothing wanted any more (the palette closed), or something else now.
    if (!preview_loaded_.empty() && preview_loaded_ != preview_want_) {
        if (preview_font_) ImGui::GetIO().Fonts->RemoveFont(preview_font_);
        preview_font_ = nullptr;
        preview_bytes_.reset();
        preview_error_.clear();
        preview_loaded_.clear();
    }
    FontLoad pl;
    // The worker only hands over the newest, which is the one asked for last.
    if (font_loader_.take_preview(pl) && !preview_want_.empty() && preview_asked_ == preview_want_) {
        bool mono = preview_want_[0] == 'm';
        preview_error_ = pl.problem;
        preview_bytes_ = pl.regular;
        if (pl.problem.empty())
            preview_font_ = mono ? load_font(pl.regular, pl.pick.regular.index, g_font_mono, g_font_mono_size)
                                 : load_font(pl.regular, pl.pick.regular.index, g_font_sans, g_font_sans_size);
        preview_loaded_ = preview_want_;
    }

    std::vector<FontFamily> fams;
    if (font_loader_.take_families(fams)) {
        auto names = [](const std::vector<FontFamily>& v) {
            std::vector<std::string> out;
            for (auto& f : v) out.push_back(f.name);
            return out;
        };
        bool changed = names(fams) != names(families_);
        families_ = std::move(fams);
        // The picker open on the old list gets the new one.
        if (changed && font_picker_open() && palette_.is_open()) palette_.update_items(font_rows(font_picker_ == 2));
    }
}

void App::install_fonts(FontLoader::Pair& in) {
    // Their problems first, whether or not the fonts change.
    bool changed = false, fresh = false;
    for (auto [key, l] : {std::pair<const char*, FontLoad*>{"font", &in.sans}, {"mono_font", &in.mono}}) {
        FontProblem& fp = font_problems_[key];
        if (fp.value == l->value && fp.problem == l->problem) continue;
        changed = true;
        fresh = fresh || !l->problem.empty();
        fp = {l->value, l->problem};
    }
    if (changed) publish_problems(fresh);

    auto key_of = [](const FontFace& f) { return path_u8(f.file) + "#" + std::to_string(f.index) + "\n"; };
    std::string key = key_of(in.sans.pick.regular) + key_of(in.sans.pick.bold) + key_of(in.mono.pick.regular) + key_of(in.mono.pick.bold);
    if (sans_ && key == fonts_key_) return;
    bool reload = sans_ != nullptr;
    fonts_key_ = key;

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    // A bold may be its regular font again: remove each only once.
    if (sans_bold_ != sans_) atlas->RemoveFont(sans_bold_);
    if (mono_bold_ != mono_) atlas->RemoveFont(mono_bold_);
    for (ImFont* f : {sans_, mono_})
        if (f) atlas->RemoveFont(f);
    // The old fonts' bytes can go now, and the new ones' must stay.
    font_bytes_ = {in.sans.regular, in.sans.bold, in.mono.regular, in.mono.bold};

    sans_ = load_font(in.sans.regular, in.sans.pick.regular.index, g_font_sans, g_font_sans_size);
    mono_ = load_font(in.mono.regular, in.mono.pick.regular.index, g_font_mono, g_font_mono_size);
    // A picked font without a semibold uses its regular face for bold, not
    // Noto's semibold, which wouldn't match it. (The editor is given none
    // then, and draws bold doubled.)
    sans_bold_ = !in.sans.regular || in.sans.bold
                     ? load_font(in.sans.bold, in.sans.pick.bold.index, g_font_sans_bold, g_font_sans_bold_size) : sans_;
    mono_bold_ = !in.mono.regular || in.mono.bold
                     ? load_font(in.mono.bold, in.mono.pick.bold.index, g_font_mono_bold, g_font_mono_bold_size) : mono_;
    if (reload) editor_.fonts_changed();
}

// Problems the rest of delta finds in a config: key chords and command ids
// (checked against the commands) and space names.
void App::check_settings(const Config& c, std::vector<ConfigIssue>& issues) const {
    std::vector<std::string> ids;
    for (auto& cmd : cmds_) ids.push_back(cmd.id);
    for (auto& [id, v] : c.keys) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            std::string near = suggest(id, ids);
            issues.push_back(issue_at(c, "keys." + id, false, near.empty() ? "no command with this name" : "no command with this name; did you mean " + near + "?"));
            continue;
        }
        std::vector<std::string> bad;
        parse_chords(v, &bad);
        for (auto& b : bad) {
            ConfigIssue is = issue_at(c, "keys." + id, true, "can't read the chord \"" + b + "\", ignored");
            // Underline just that chord.
            auto w = c.where.find("keys." + id);
            if (w != c.where.end()) {
                size_t at = v.find(b);
                if (at != std::string::npos) { is.col0 = w->second.val0 + (int)at; is.col1 = is.col0 + (int)b.size(); }
            }
            issues.push_back(is);
        }
    }
    if (!Store::check_name(c.default_space, "space").empty())
        issues.push_back(issue_at(c, "general.default_space", true, "not a valid folder name, using default"));
    if (!Store::check_name(c.daily_space, "space").empty())
        issues.push_back(issue_at(c, "general.daily_space", true, "not a valid folder name, using daily"));

    // Dictionaries: built in, or a pair of files in <config dir>/dictionaries.
    std::vector<std::string> dicts = Speller::available(dict_dir_);
    auto check_langs = [&](const std::string& key, const std::string& v) {
        for (auto& l : split_list(v)) {
            if (std::any_of(dicts.begin(), dicts.end(), [&](const std::string& d) { return iequals(d, l); })) continue;
            std::string near = iequals(l, "en_UK") ? "en_GB" : suggest(l, dicts);  // en_US is nearer by letters
            ConfigIssue is = issue_at(c, key, true, near.empty() ? "no dictionary with this name; add " + l + ".aff and " + l + ".dic to " + path_u8(dict_dir_)
                                                                 : "no dictionary with this name; did you mean " + near + "?");
            // Underline just that name.
            auto w = c.where.find(key);
            if (w != c.where.end() && is.line >= 0) {
                size_t at = v.find(l);
                if (at != std::string::npos) { is.col0 = w->second.val0 + (int)at; is.col1 = is.col0 + (int)l.size(); }
            }
            issues.push_back(is);
        }
    };
    check_langs("spell.languages", c.spell_languages);
    Store notes;
    notes.set_root(expand_user(c.notes_dir));
    std::vector<std::string> spaces = notes.spaces();
    std::error_code ec;
    bool have_notes = fs::is_directory(notes.root(), ec);  // else notes_dir says what's wrong
    for (auto& [space, v] : c.spell_spaces) {
        std::string key = "spell.space." + space;
        check_langs(key, v);
        if (!have_notes || std::any_of(spaces.begin(), spaces.end(), [&](const std::string& s) { return iequals(s, space); })) continue;
        std::vector<std::string> lowered;
        for (auto& s : spaces) lowered.push_back(lower(s));
        std::string near = suggest(space, lowered);
        issues.push_back(issue_at(c, key, false, near.empty() ? "no space with this name" : "no space with this name; did you mean space." + near + "?"));
    }
}

void App::set_problems(std::vector<ConfigIssue> issues) {
    config_problems_ = std::move(issues);
    if (is_config()) refresh_problems();
}

// Gathers the open document's problems from the checks that apply to it:
// the config's own, or those found in a note. Kept in line order, for the
// count and F8.
void App::refresh_problems() {
    problems_.clear();
    if (is_config()) {
        for (auto& is : config_problems_)
            if (is.line >= 0) problems_.push_back({is.line, is.col0, is.col1, is.msg});
    } else if (note_) {
        problems_ = link_problems(editor_.lines(), note_index_);
    }
    std::stable_sort(problems_.begin(), problems_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col0 < b.col0;
    });
    diag_gen_++;
}

// The words in words.txt, one a line ("#" starts a comment line).
static std::vector<std::string> read_words(const fs::path& path) {
    std::vector<std::string> out;
    std::string text;
    if (!read_file(path, text)) return out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

// Loads the dictionaries the open space asks for, if they aren't what's
// loaded already (or `force`: words.txt changed).
void App::apply_spelling(bool force) {
    std::string langs = cfg_.spell_languages;
    if (auto it = cfg_.spell_spaces.find(lower(space_)); it != cfg_.spell_spaces.end()) langs = it->second;
    std::string key = cfg_.spell ? langs : "";
    if (!force && key == spell_key_) return;
    // The same dictionaries again (words.txt changed): the marks stay up
    // until the reload is in, rather than blinking out.
    if (key != spell_key_) {
        spelling_.clear();
        diag_gen_++;
    }
    spell_key_ = key;
    spell_version_ = UINT64_MAX;
    spell_pending_ = false;
    words_mtime_ = mtime_of(words_path_);
    if (!cfg_.spell) { speller_.unload(); return; }
    std::vector<std::string> words = read_words(words_path_);
    speller_.load(split_list(langs), dict_dir_, words);
    spell_report_ = true;
}

// Spelling marks for the open note. New lines are checked a few
// milliseconds' worth a frame, so a long note never stalls one.
void App::check_spelling() {
    if (!is_note() || !cfg_.spell || !speller_.ready()) return;
    if (spell_report_) {
        spell_report_ = false;
        if (!speller_.errors().empty()) status("dictionary " + speller_.errors().front(), true);
    }
    if (editor_.version() == spell_version_ && speller_.generation() == spell_gen_ && !spell_pending_) return;
    spell_version_ = editor_.version();
    spell_gen_ = speller_.generation();
    spell_pending_ = !speller_.check(editor_.lines(), spelling_, 0.004);
    diag_gen_++;
}

// The misspelt word being typed isn't marked until the caret leaves it.
// Returns its start (line -1: none).
TextPos App::spell_held() {
    if (editor_.version() != edit_version_) {
        edit_version_ = editor_.version();
        spell_hold_ = editor_.cursor();
    }
    if (spell_hold_.line < 0) return {-1, 0};
    TextPos c = editor_.cursor();
    for (auto& d : spelling_)
        if (d.line == spell_hold_.line && d.col0 <= spell_hold_.col && spell_hold_.col <= d.col1) {
            if (c.line == d.line && d.col0 <= c.col && c.col <= d.col1) return {d.line, d.col0};
            break;
        }
    if (c != spell_hold_) spell_hold_.line = -1;  // moved on
    return {-1, 0};
}

// Hands the problems and spelling marks to the editor, when they've changed.
// Problems on the line being typed on wait for a pause, so a half-typed
// value or [[link]] doesn't flash at you; a word waits for you to finish it.
void App::show_problems(double now) {
    bool typing = now - last_change_ < 0.9;
    if (typing) next_problem_check_ = last_change_ + 0.9;
    int hold = typing ? editor_.cursor().line : -1;
    TextPos held = spell_held();
    if (diag_gen_ == shown_gen_ && hold == shown_hold_ && held == shown_held_) return;
    shown_gen_ = diag_gen_;
    shown_hold_ = hold;
    shown_held_ = held;
    std::vector<Diagnostic> d;
    for (auto& p : problems_)
        if (p.line != hold) d.push_back(p);
    for (auto& p : spelling_)
        if (p.line != held.line || p.col0 != held.col) d.push_back(p);
    editor_.set_diagnostics(d);
}

const Diagnostic* App::misspelt_at_caret() const {
    int i = misspelling_at(spelling_, editor_.cursor());
    return i >= 0 ? &spelling_[i] : nullptr;
}

// What the status line echoes about the caret's spot: a misspelt word's
// likeliest fixes, or the message of a problem in a note. Nothing pops up
// over the text; it's there if you look down.
void App::update_echo() {
    echo_.clear();
    if (!note_ || editor_.reader()) return;
    TextPos c = editor_.cursor();
    if (const Diagnostic* d = misspelt_at_caret(); d && !(shown_held_.line == d->line && shown_held_.col == d->col0)) {
        // Only the best guess ("Disempower?"); the key for the rest (and for
        // keeping the word) is an aside.
        // Found in the background (they can take tens of milliseconds): the
        // echo shows once they're in.
        const std::string& s = editor_.lines()[d->line];
        auto found = speller_.suggest_later(spell_form(s, {d->col0, d->col1}));
        if (!found) return;
        const auto& sug = *found;
        using Act = Echo::Act;
        if (sug.empty()) {
            echo_.push_back({"no suggestions", Tone::Plain});
        } else {
            echo_.push_back({sug[0], Tone::Strong, Act::QuickFix});
            echo_.push_back({"?", Tone::Plain, Act::QuickFix});
        }
        // The keys: "alt+. fix · ctrl+. more" (either may be unbound).
        std::string quick = sug.empty() ? "" : chord_for("edit.quick_fix");
        std::string more = chord_for("edit.fix_spelling");
        if (!quick.empty() || !more.empty()) echo_.push_back({"   ", Tone::Quiet});
        if (!quick.empty()) echo_.push_back({quick + " fix", Tone::Quiet, Act::QuickFix});
        if (!quick.empty() && !more.empty()) echo_.push_back({" \xC2\xB7 ", Tone::Quiet});
        if (!more.empty()) echo_.push_back({quick.empty() ? more : more + " more", Tone::Quiet, Act::FixSpelling});
    } else if (!is_config() && c.line != shown_hold_) {
        // The config's problems say it at the ends of their lines, which are
        // short and right beside them.
        for (auto& p : problems_)
            if (p.line == c.line && p.col0 >= 0 && p.col0 <= c.col && c.col <= p.col1) {
                echo_.push_back({p.msg, Tone::Strong});
                break;
            }
    }
}

void App::goto_misspelling(int dir) {
    Jump j(*this, History::Misspelling);
    if (spelling_.empty()) {
        status(!cfg_.spell ? "spellcheck is off" : speller_.loading() || spell_pending_ ? "still checking the spelling" : "no misspelt words in this note");
        return;
    }
    TextPos a = editor_.cursor(), b = editor_.anchor();
    TextPos from = dir > 0 ? std::max(a, b) : std::min(a, b);
    const Diagnostic* to = nullptr;
    if (dir > 0) {
        for (auto& d : spelling_)
            if (from < TextPos{d.line, d.col0}) { to = &d; break; }
        if (!to) to = &spelling_.front();  // wrap
    } else {
        for (auto it = spelling_.rbegin(); it != spelling_.rend(); ++it)
            if (TextPos{it->line, it->col0} < from) { to = &*it; break; }
        if (!to) to = &spelling_.back();
    }
    editor_.select_range({to->line, to->col0}, {to->line, to->col1});
}

// The quick fix: the misspelt word at the caret, or the nearest one before
// it in the same paragraph, takes its best guess, and the caret stays where
// it was. Pressed again straight away, the next guess goes in instead (and
// at the end of them, the word as it was); one undo puts the word back,
// whichever it's on.
void App::quick_fix() {
    if (!is_note() || editor_.reader()) return;
    check_spelling();  // marks for the text as it is now, not last frame's
    if (quick_.line >= 0 && editor_.version() == quick_.version && editor_.cursor() == quick_.caret) {
        quick_.pick = (quick_.pick + 1) % (int)quick_.options.size();
        editor_.undo();  // back to the word as written, then the next guess
        quick_apply();
        return;
    }
    quick_.line = -1;
    TextPos c = editor_.cursor();
    const auto& lines = editor_.lines();
    int near = misspelling_near(spelling_, lines, c);
    const Diagnostic* d = near >= 0 ? &spelling_[near] : nullptr;
    if (!d || d->col1 > (int)lines[d->line].size()) { status(cfg_.spell ? "no misspelt word here" : "spellcheck is off"); return; }
    std::string word = lines[d->line].substr(d->col0, d->col1 - d->col0);
    const auto& sugg = speller_.suggest(spell_form(lines[d->line], {d->col0, d->col1}));
    if (sugg.empty()) { status("no suggestions for " + word); return; }
    quick_.line = d->line;
    quick_.col0 = d->col0;
    quick_.word = word;
    quick_.options = sugg;
    quick_.options.push_back(word);  // the cycle ends on the word as it was
    quick_.pick = 0;
    quick_.from = c;
    quick_apply();
}

// Puts the picked guess in place of the word, the caret back where it was
// (moved along if the word before it changed length).
void App::quick_apply() {
    const std::string& to = quick_.options[quick_.pick];
    int end = quick_.col0 + (int)quick_.word.size();
    editor_.replace_range({quick_.line, quick_.col0}, {quick_.line, end}, to);
    TextPos back = after_replace(quick_.from, quick_.line, quick_.col0, (int)quick_.word.size(), (int)to.size());
    editor_.jump_to(back);
    quick_.caret = back;
    quick_.version = editor_.version();
    // No message: the change pulses where it happened, and a message would
    // cover the echo for the next word.
}

// Fixing spelling happens in the status line, a word at a time. The field
// holds the fix: the best guess to start with, tab for the others, or type
// your own. Enter fixes the word and goes on to the next, shift+enter skips
// it, ctrl+enter adds it to your dictionary; the pass ends where it began,
// or at escape. From a word that isn't misspelt, it starts at the next one.
void App::fix_spelling() {
    if (!is_note()) return;
    if (editor_.reader()) editor_.set_reader(false);
    check_spelling();  // marks for the text as it is now, not last frame's
    const Diagnostic* d = misspelt_at_caret();
    if (!d) {
        goto_misspelling(1);  // says why not, if there's nowhere to go
        if (!(d = misspelt_at_caret())) return;
    }
    fix_origin_ = {d->line, d->col0};
    fix_wrapped_ = fix_advance_ = false;
    open_bar(Bar::Fix);
    fix_load(*d);
}

// Takes up a misspelt word: selected in the note, its best guess in the field.
void App::fix_load(const Diagnostic& d) {
    const std::string& s = editor_.lines()[d.line];
    fix_.line = d.line;
    fix_.col0 = d.col0;
    fix_.col1 = d.col1;
    fix_.word = s.substr(d.col0, d.col1 - d.col0);
    fix_.sugg = speller_.suggest(spell_form(s, {d.col0, d.col1}));
    fix_.pick = 0;
    fix_field(fix_.sugg.empty() ? fix_.word : fix_.sugg[0]);
    editor_.select_range({d.line, d.col0}, {d.line, d.col1});
}

// Puts text in the fix field, selected, so typing replaces it.
void App::fix_field(const std::string& text) {
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", text.c_str());
    bar_set_ = text;  // for when the field is active (see bar_input_callback)
    bar_set_pending_ = true;
    bar_focus_ = true;
    bar_select_pending_ = 1;
}

// On to the next misspelt word from `from`, once the spelling has caught up
// with the edit just made (see draw_fix_bar).
void App::fix_next(TextPos from) {
    fix_from_ = from;
    fix_advance_ = true;
}

void App::add_to_dictionary(const std::string& word) {
    std::string text, err;
    read_file(words_path_, text);
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += word + "\n";
    std::error_code ec;
    fs::create_directories(words_path_.parent_path(), ec);
    if (!atomic_write(words_path_, text, &err)) { status("couldn't save words.txt: " + err, true); return; }
    words_mtime_ = mtime_of(words_path_);  // already applied below: no reload
    speller_.add_word(word);
    status("added \"" + word + "\" to your dictionary");
}

// Steps through the open document's problems. From a note that has none, the
// config's are next door.
void App::goto_problem(int dir) {
    Jump j(*this, History::Problem);
    const Diagnostic* to = nullptr;
    if (problems_.empty()) {
        bool config_has = std::any_of(config_problems_.begin(), config_problems_.end(), [](const ConfigIssue& is) { return is.line >= 0; });
        if (is_config() || !config_has) { status(is_config() ? "no problems in the config" : "no problems in this note"); return; }
        open_config();
        if (!is_config() || problems_.empty()) return;
        to = dir > 0 ? &problems_.front() : &problems_.back();
    }
    TextPos a = editor_.cursor(), b = editor_.anchor();
    TextPos from = dir > 0 ? std::max(a, b) : std::min(a, b);
    auto pos = [](const Diagnostic& d) { return TextPos{d.line, std::max(0, d.col0)}; };
    if (to) {
        // just opened: the first (or last) one
    } else if (dir > 0) {
        for (auto& d : problems_)
            if (from < pos(d)) { to = &d; break; }
        if (!to) to = &problems_.front();  // wrap
    } else {
        for (auto it = problems_.rbegin(); it != problems_.rend(); ++it)
            if (pos(*it) < from) { to = &*it; break; }
        if (!to) to = &problems_.back();
    }
    // The part it's about comes selected, ready to retype.
    TextPos s{to->line, std::max(0, to->col0)}, e{to->line, to->col0 >= 0 ? to->col1 : 0};
    editor_.select_range(s, e);
}

void App::open_config() {
    load_settings();  // makes sure the file exists
    NoteRef r;
    r.space = "config";
    r.name = path_u8(cfg_path_.filename());
    r.path = cfg_path_;
    open_note(r);
    if (status_error_) status_.clear();  // the problems are on their lines now
}

// Picks a setting the config doesn't set yet and writes it in with its
// default, value selected, ready to change.
void App::add_setting() {
    if (!is_config()) open_config();
    if (!is_config()) return;
    std::set<std::string> have;
    for (auto& e : scan_config(editor_.lines())) have.insert(e.section + "." + e.key);

    auto insert = [this](std::string section, std::string key, std::string value) {
        if (!is_config()) return;
        ConfigEdit e = config_set(editor_.lines(), section, key, value);  // not set yet: added
        TextPos end = editor_.insert_text({e.line, e.col0}, e.text);
        editor_.select_range({end.line, end.col - (int)value.size()}, end);
    };

    std::vector<PaletteItem> items;
    for (auto& s : all_settings()) {
        std::string id = std::string(s.section) + "." + s.key;
        if (have.count(id)) continue;
        PaletteItem it;
        it.label = id;
        std::string def = s.default_for(cfg_.theme);
        it.meta = (*s.desc ? std::string(s.desc) + ", " : std::string()) + "default " + def;
        it.action = [insert, sec = std::string(s.section), key = std::string(s.key), def] { insert(sec, key, def); };
        items.push_back(std::move(it));
    }
    for (auto& c : cmds_) {
        if (have.count("keys." + c.id)) continue;
        PaletteItem it;
        it.label = "keys." + c.id;
        it.meta = c.title;
        it.action = [insert, id = c.id, def = c.defaults] { insert("keys", id, def); };
        items.push_back(std::move(it));
    }
    palette_.pick("add setting: ", std::move(items));
}

// The font picker's rows, from the installed fonts as last listed.
std::vector<PaletteItem> App::font_rows(bool mono) {
    const char* key = mono ? "mono_font" : "font";
    const std::string builtin = find_setting("editor", key)->def;
    const std::string cur = mono ? cfg_.mono_font : cfg_.font;
    std::vector<PaletteItem> items;
    auto row = [&](const std::string& label, const std::string& meta) {
        PaletteItem it;
        it.label = label;
        it.current = iequals(label, cur) || (cur.empty() && label == builtin);
        it.meta = !it.current ? meta : meta.empty() ? "current" : meta + ", current";
        it.action = [this, mono, label] { set_font(mono, label); };
        items.push_back(std::move(it));
    };
    row(builtin, "built in");
    if (is_font_path(cur)) row(cur, "file");
    for (auto& f : families_) {
        // Not on this computer as a file (a cloud font), or the built-in one again.
        if ((mono && !f.mono) || f.regular.file.empty() || iequals(f.name, builtin)) continue;
        row(f.name, real_bold(f.regular, f.bold) ? "" : "no bold");
    }
    return items;
}

// Picks an installed font for the config's font or mono_font (or, typed, a
// font file), with the one selected shown in itself under the list. The
// list is the one from last time (or start-up), and is listed again on the
// font worker: a font installed since joins it when that's done.
void App::choose_font(bool mono) {
    font_loader_.list_families();
    Palette::PickOptions opts;
    opts.typed = [this, mono](const std::string& text, PaletteItem& it) {
        if (text.empty() || !is_font_path(text)) return false;
        it.label = text;
        it.meta = "use this file";
        it.action = [this, mono, text] { set_font(mono, text); };
        return true;
    };
    opts.preview = [this, mono](const PaletteItem* sel, ImDrawList* dl, ImVec2 pos, float w, float h, float alpha) {
        (void)w;  // clipped to it already
        std::string value = sel ? sel->label : std::string();
        double t = ImGui::GetTime();
        if (value != preview_sel_) {
            preview_sel_ = value;
            preview_since_ = t;
        }
        std::string want = value.empty() ? std::string() : (mono ? "m:" : "s:") + value;
        if (t - preview_since_ >= 0.12) preview_want_ = want;
        else anim::active = true;  // a frame when it has rested long enough
        if (want.empty() || preview_loaded_ != want) return;

        float s = base_size(), size = mono ? std::round(s * cfg_.code_size) : s;  // as the editor sets them
        float y = std::round(pos.y + (h - size) * 0.5f);
        if (!preview_error_.empty()) {
            dl->AddText(mono_, std::round(s * cfg_.ui_size), ImVec2(pos.x, y), col_alpha(T.dim, alpha), preview_error_.c_str());
            return;
        }
        if (!preview_font_) return;
        const char* sample = mono ? "if (x == 0O1lI) return {\"\xE2\x86\x92\", 42}; // \xE2\x9C\x93"
                                  : "The quick brown fox jumps over the lazy dog. 0O 1lI \xE2\x86\x92";
        dl->AddText(preview_font_, size, ImVec2(pos.x, y), col_alpha(T.text, alpha), sample);
    };
    palette_.pick(mono ? "mono font: " : "font: ", font_rows(mono), std::move(opts));
    font_picker_ = mono ? 2 : 1;
    font_picker_gen_ = palette_.generation();
}

// Writes the font picker's choice into the config; the reload that follows
// loads the font.
void App::set_font(bool mono, const std::string& value) {
    status(std::string(mono ? "mono font: " : "font: ") + value);
    set_setting("editor", mono ? "mono_font" : "font", value);
}

// Writes one setting into the config: into the editor as one undoable edit
// when the config is open there, else into the file. Either way it's saved
// and reloaded.
void App::set_setting(const std::string& section, const std::string& key, const std::string& value) {
    if (is_config()) {
        ConfigEdit e = config_set(editor_.lines(), section, key, value);
        editor_.replace_range({e.line, e.col0}, {e.line, e.col1}, e.text);
        save();
        return;
    }
    std::string text;
    read_file(cfg_path_, text);  // none yet: written from scratch
    bool crlf = text.find("\r\n") != std::string::npos;
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (text.empty() || text.back() == '\n') lines.emplace_back();  // the empty last line after a final newline
    apply_edit(lines, config_set(lines, section, key, value));
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) out += (i ? (crlf ? "\r\n" : "\n") : "") + lines[i];
    std::string err;
    if (!atomic_write(cfg_path_, out, &err)) {
        status("couldn't save the config: " + err, true);
        return;
    }
    load_settings();
}

// Picks a theme, showing each as the selection lands on it. Enter keeps it
// (into the config); leaving any other way puts the config's back.
void App::choose_theme() {
    std::vector<PaletteItem> items;
    for (auto& t : all_themes()) {
        PaletteItem it;
        it.label = t.name;
        it.current = t.name == cfg_.theme;
        it.meta = std::string(t.dark() ? "dark" : "light") + (it.current ? ", current" : "");
        it.action = [this, name = t.name] {
            status("theme: " + name);
            set_setting("appearance", "theme", name);
        };
        items.push_back(std::move(it));
    }
    Palette::PickOptions opts;
    opts.select = [this](const PaletteItem* sel) {
        if (sel && sel->label != theme_shown_) show_theme(sel->label);
    };
    palette_.pick("theme: ", std::move(items), std::move(opts));
    theme_picker_gen_ = palette_.generation();
}

void App::show_theme(const std::string& name) {
    theme_shown_ = name;
    build_theme(cfg_, &name);
    apply_imgui_style();
    style_native_window(win_, col_rgb(T.bg), col_rgb(T.dim), T.dark);
    anim::active = true;  // drawn in it next frame
}

std::vector<BindingDoc> App::binding_docs() const {
    std::vector<BindingDoc> out;
    for (auto& c : cmds_) out.push_back({c.id, c.title, c.defaults});
    return out;
}

App::Command* App::find_command(const std::string& id) {
    for (auto& c : cmds_)
        if (c.id == id) return &c;
    return nullptr;
}

std::string App::chord_for(const std::string& id) const {
    for (auto& c : cmds_)
        if (c.id == id && !c.chords.empty()) return chord_str(c.chords.front());
    return {};
}

// Unreadable chords are skipped (check_settings reports them).
void App::apply_bindings() {
    for (auto& c : cmds_) {
        auto it = cfg_.keys.find(c.id);
        c.chords = parse_chords(it != cfg_.keys.end() ? it->second : c.defaults);
    }
}

void App::register_commands() {
    auto add = [&](const char* id, const char* title, const char* keys, std::function<void()> fn, bool repeat = false, bool global = false) {
        Command c;
        c.id = id;
        c.title = title;
        c.defaults = keys;
        c.run = std::move(fn);
        c.repeat = repeat;
        c.global = global;
        cmds_.push_back(std::move(c));
    };
    auto ed = [this](void (Editor::*fn)()) { return [this, fn] { if (editing()) (editor_.*fn)(); }; };

    // Navigation
    add("palette.notes", "find note in this space", "ctrl+o", [this] { open_palette("."); }, false, true);
    add("palette.all", "find note in any space", "ctrl+alt+o", [this] { open_palette(".", true); }, false, true);
    add("palette.sort", "sort notes by name or recent", "ctrl+r", [this] {
        sort_recent_ = !sort_recent_;
        if (palette_.in_mode('.')) palette_.rebuild();
        else status(sort_recent_ ? "notes sorted by recent" : "notes sorted by name");
    }, false, true);
    add("palette.commands", "show all commands", "ctrl+p", [this] { open_palette(""); }, false, true);
    add("palette.spaces", "switch space", "ctrl+shift+s", [this] { open_palette("/"); }, false, true);
    add("palette.headings", "jump to heading", "ctrl+shift+o", [this] { open_palette("@"); }, false, true);
    add("palette.line", "go to line", "ctrl+g", [this] { open_line(); }, false, true);
    add("find.open", "find in this note", "ctrl+f", [this] { open_find(false); }, false, true);
    add("find.replace", "find and replace in this note", "ctrl+h", [this] { open_find(true); }, false, true);
    add("nav.heading_next", "next heading", "ctrl+alt+down", [this] {
        if (!editing()) return;
        Jump j(*this, History::Heading);
        editor_.goto_heading(1);
    }, true);
    add("nav.heading_prev", "previous heading", "ctrl+alt+up", [this] {
        if (!editing()) return;
        Jump j(*this, History::Heading);
        editor_.goto_heading(-1);
    }, true);
    add("edit.expand_selection", "expand selection", "alt+shift+right", [this] { if (editing()) editor_.expand_selection(); }, true);
    add("edit.shrink_selection", "shrink selection", "alt+shift+left", [this] { if (editing()) editor_.shrink_selection(); }, true);
    add("edit.follow_link", "follow link at caret", "alt+enter", [this] {
        LinkSpan link;
        if (!editing()) return;
        if (editor_.link_at_caret(link)) open_link(link);
        else status("no link at the caret");
    });
    add("edit.web_search", "search the web for selection", "alt+shift+enter", [this] {
        if (!note_ || !editor_.has_selection()) status("select some text to search the web for it");
        else if (!web_search(editor_.selected_text())) status("couldn't open a browser; set a default browser in your system settings", true);
    });
    add("palette.search", "search notes in this space", "ctrl+shift+f", [this] {
        std::string sel = note_ && editor_.has_selection() && editor_.cursor().line == editor_.anchor().line ? editor_.selected_text() : "";
        open_palette("?" + sel);
    }, false, true);
    add("palette.backlinks", "show notes that link here", "ctrl+shift+b", [this] { open_palette("["); }, false, true);
    add("nav.back", "go back", "alt+left", [this] { history_step(-1); });
    add("nav.forward", "go forward", "alt+right", [this] { history_step(1); });
    add("nav.switch", "switch to recent note", "ctrl+tab", [this] { switch_note(1); }, true);
    add("nav.switch_back", "switch to recent note, oldest first", "ctrl+shift+tab", [this] { switch_note(-1); }, true);

    // Notes and spaces
    add("note.new", "new note", "ctrl+n", [this] {
        palette_.prompt("new note: " + space_ + "/", "", "+ new note", [this](const std::string& v) { create_note(space_, v); });
    });
    add("note.daily", "open today's daily note", "alt+d", [this] { open_daily(); });
    add("note.rename", "rename note", "f2", [this] { rename_note(); });
    add("note.move", "move note to another space", "", [this] { move_note(); });
    add("note.delete", "delete note", "", [this] { delete_note(); });
    add("note.restore", "restore deleted note", "", [this] { restore_note(); });
    add("note.save", "save note now", "ctrl+s", [this] { if (save(true)) status("saved"); });
    add("note.reload", "reload note", "", [this] { reload_note(); });
    add("space.new", "new space", "", [this] { new_space(); });
    add("space.rename", "rename this space", "", [this] { rename_space(); });
    add("space.delete", "delete this space", "", [this] { delete_space(); });

    // View
    add("view.reader", "toggle reader mode", "ctrl+e", [this] {
        if (!note_ || is_config()) return;
        if (is_manual()) { status("the manual is read only"); return; }
        editor_.set_reader(!editor_.reader());  // morphs between the views
    }, false, false);
    add("view.line_numbers", "toggle line numbers", "", [this] { cfg_.line_numbers = !cfg_.line_numbers; });
    add("view.zoom_in", "zoom in", "ctrl+= ctrl+numpad+", [this] { zoom_ = std::min(3.0f, zoom_ * 1.1f); status("zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_out", "zoom out", "ctrl+- ctrl+numpad-", [this] { zoom_ = std::max(0.5f, zoom_ / 1.1f); status("zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_reset", "reset zoom", "ctrl+0", [this] { zoom_ = 1; status("zoom 100%"); }, false, true);
    add("view.fullscreen", "toggle full screen", "f11", [this] { toggle_fullscreen(); }, false, true);

    // App
    add("config.open", "edit config", "ctrl+,", [this] { open_config(); });
    add("config.add_setting", "add setting", "", [this] { add_setting(); });
    add("font.choose", "choose font", "", [this] { choose_font(false); });
    add("font.choose_mono", "choose mono font", "", [this] { choose_font(true); });
    add("theme.choose", "choose theme", "", [this] { choose_theme(); });
    add("config.open_external", "open config in another app", "", [this] {
        if (!open_with_default_app(cfg_path_)) status("couldn't open " + path_u8(cfg_path_.filename()) + "; choose a default app for its file type in your system settings", true);
    });
    add("nav.next_problem", "next problem", "f8", [this] { goto_problem(1); }, true);
    add("nav.prev_problem", "previous problem", "shift+f8", [this] { goto_problem(-1); }, true);
    add("nav.next_misspelling", "next misspelt word", "alt+f8", [this] { goto_misspelling(1); }, true);
    add("nav.prev_misspelling", "previous misspelt word", "alt+shift+f8", [this] { goto_misspelling(-1); }, true);
    add("edit.quick_fix", "quick fix misspelt word", "alt+.", [this] { quick_fix(); });
    add("edit.fix_spelling", "fix spelling, word by word", "ctrl+.", [this] { fix_spelling(); });
    add("view.spelling", "toggle spellcheck", "", [this] {
        cfg_.spell = !cfg_.spell;
        apply_spelling();
        status(cfg_.spell ? "spellcheck on" : "spellcheck off");
    });
    add("config.reload", "reload config", "", [this] { load_settings(); status("config reloaded"); });
    add("app.open_folder", "show this space's folder", "", [this] { open_with_default_app(store_.root() / u8path(space_)); });
    add("app.manual", "open manual", "f1", [this] { open_manual(); });
    add("app.quit", "quit", "ctrl+q", [this] { quit_ = true; }, false, true);

    // Editing
    add("edit.undo", "undo", "ctrl+z", ed(&Editor::undo), true);
    add("edit.redo", "redo", "ctrl+y ctrl+shift+z", ed(&Editor::redo), true);
    add("edit.cut", "cut", "ctrl+x", [this] { if (editing()) copy_out(true); });
    add("edit.copy", "copy", "ctrl+c", [this] { if (note_) copy_out(false); });
    add("edit.paste", "paste", "ctrl+v", [this] { paste(false); }, true);
    add("edit.paste_plain", "paste as plain text", "ctrl+shift+v", [this] { paste(true); }, true);
    add("image.view", "view image", "", [this] {
        int tile = 0;
        int line = editor_.image_for_caret(&tile);
        if (line >= 0) open_viewer(line, tile);
        else status("pick an image to view it");
    });
    add("edit.select_all", "select all", "ctrl+a", ed(&Editor::select_all));
    add("edit.select_line", "select line", "ctrl+l", ed(&Editor::select_line), true);
    add("edit.line_up", "move line up", "alt+up", [this] { if (editing()) editor_.move_lines(-1); }, true);
    add("edit.line_down", "move line down", "alt+down", [this] { if (editing()) editor_.move_lines(1); }, true);
    add("edit.duplicate_up", "duplicate line up", "shift+alt+up", [this] { if (editing()) editor_.duplicate_lines(-1); }, true);
    add("edit.duplicate_down", "duplicate line down", "shift+alt+down", [this] { if (editing()) editor_.duplicate_lines(1); }, true);
    add("edit.delete_line", "delete line", "ctrl+shift+k", ed(&Editor::delete_lines), true);
    add("edit.toggle_task", "toggle task checkbox", "ctrl+enter", ed(&Editor::toggle_task));
    add("edit.bold", "bold", "ctrl+b", [this] { if (editing()) editor_.wrap_selection("**"); });
    add("edit.italic", "italic", "ctrl+i", [this] { if (editing()) editor_.wrap_selection("*"); });
    add("edit.code", "inline code", "ctrl+`", [this] { if (editing()) editor_.wrap_selection("`"); });
    add("edit.strike", "strikethrough", "ctrl+shift+x", [this] { if (editing()) editor_.wrap_selection("~~"); });
    add("edit.indent", "indent", "ctrl+]", ed(&Editor::indent), true);
    add("edit.outdent", "outdent", "ctrl+[", ed(&Editor::outdent), true);

    // The palette mode each opens, shown beside its shortcut in the list so
    // the prefixes can be learnt there.
    for (auto [id, p] : {std::pair{"palette.notes", '.'}, {"palette.all", '.'}, {"palette.spaces", '/'},
                         {"palette.headings", '@'}, {"palette.line", ':'}, {"palette.search", '?'}, {"palette.backlinks", '['}})
        if (Command* c = find_command(id)) c->prefix = p;
}

// ---------------------------------------------------------------------------
// Notes

void App::set_title() {
    std::string t = note_ ? note_->name + " - delta" : "delta";
    if (sandbox_) t += " (sandbox)";
    glfwSetWindowTitle(win_, t.c_str());
}

void App::remember_view() {
    if (note_) views_[path_u8(note_->path)] = {editor_.cursor(), editor_.scroll()};
}

std::string App::disk_text() const {
    std::string text = editor_.text();
    // A download still on its way is saved as the address it came from; the
    // placeholder is only ever on screen.
    for (auto& d : downloads_) {
        if (d->note != note_->path) continue;
        for (size_t at = text.find(d->marker); at != std::string::npos; at = text.find(d->marker, at))
            text.replace(at, d->marker.size(), d->url);
    }
    if (crlf_) {
        std::string out;
        out.reserve(text.size() + text.size() / 20);
        for (char c : text) {
            if (c == '\n') out += '\r';
            out += c;
        }
        text.swap(out);
    }
    return text;
}

bool App::save(bool over_disk) {
    if (!note_ || is_manual() || editor_.version() == saved_version_) return true;
    if (!over_disk && !conflict_) {
        // Changed on disk since we last looked (before the poll saw it).
        auto nm = mtime_of(note_->path);
        if (nm != fs::file_time_type{} && disk_mtime_ != fs::file_time_type{} && nm != disk_mtime_) {
            disk_mtime_ = nm;
            conflict_ = true;
        }
    }
    if (conflict_ && !over_disk) return false;
    std::string err;
    std::error_code ec;
    fs::create_directories(note_->path.parent_path(), ec);
    if (!atomic_write(note_->path, disk_text(), &err)) {
        status("couldn't save: " + err, true);
        return false;
    }
    saved_version_ = editor_.version();
    disk_mtime_ = mtime_of(note_->path);
    conflict_ = false;
    if (is_config()) load_settings();  // edits apply as soon as they're saved
    return true;
}

// The note as it is on disk, in place of what's in the editor.
void App::reload_note() {
    if (!note_ || is_manual()) return;
    std::string text;
    if (!read_file(note_->path, text)) {
        status("couldn't read " + note_->name + "; it may have been moved or deleted", true);
        return;
    }
    crlf_ = text.find("\r\n") != std::string::npos;
    editor_.reload_text(text);
    saved_version_ = seen_version_ = editor_.version();
    disk_mtime_ = mtime_of(note_->path);
    conflict_ = false;
}

// Leaving a note in conflict with disk: the file keeps the other version,
// and your edits are saved beside it as "name (yours)", so neither is lost.
void App::keep_yours() {
    if (!conflict_ || !note_ || editor_.version() == saved_version_) { conflict_ = false; return; }
    conflict_ = false;
    fs::path dir = note_->path.parent_path(), ext = note_->path.extension();
    std::string stem = path_u8(note_->path.stem()) + " (yours)", name = stem;
    std::error_code ec;
    for (int i = 2; fs::exists(dir / u8path(name + path_u8(ext)), ec); i++) name = stem + " " + std::to_string(i);
    std::string err;
    if (!atomic_write(dir / u8path(name + path_u8(ext)), disk_text(), &err)) {
        status("couldn't keep your edits: " + err, true);
        return;
    }
    saved_version_ = editor_.version();  // nothing left to save over the other version
    status("kept your edits as " + name + "; " + note_->name + " has the version from disk");
}

void App::open_note(const NoteRef& n, bool push_history) {
    std::optional<Jump> jump;
    if (push_history) jump.emplace(*this);
    close_bar();
    keep_yours();
    save();
    remember_view();
    bool config = n.path == cfg_path_, manual = n.path == kManual;
    std::string text;
    if (manual) text.assign((const char*)g_manual, g_manual_size);
    else if (!read_file(n.path, text)) {
        status("couldn't open " + n.name + "; it may have been moved, deleted or locked by another app", true);
        return;
    }
    crlf_ = text.find("\r\n") != std::string::npos;

    // The config is edited like a note, but as ini text and never in reader
    // mode; the manual is only ever in reader mode. The reader preference
    // comes back when you return to notes.
    bool was_fixed = is_config() || is_manual();
    if ((config || manual) && !was_fixed) reader_pref_ = editor_.reader();
    if (config || manual) editor_.set_reader(manual, false);
    else if (was_fixed) editor_.set_reader(reader_pref_, false);
    editor_.set_syntax(config ? Syntax::Ini : Syntax::Markdown);
    images_.set_note_dir(n.path.parent_path());  // images are found from the note's folder
    editor_.set_text(text);
    note_ = n;
    if (!config && !manual) {
        space_ = n.space;
        note_index_ = store_.index();  // a note just made by following a link counts at once
    }
    refresh_problems();
    spelling_.clear();
    spell_version_ = UINT64_MAX;
    spell_pending_ = false;
    edit_version_ = editor_.version();  // opening isn't typing
    spell_hold_.line = -1;
    apply_spelling();                    // this space may want other dictionaries
    check_spelling();
    show_problems(ImGui::GetTime());
    hints_version_ = UINT64_MAX;
    saved_version_ = seen_version_ = editor_.version();
    disk_mtime_ = mtime_of(n.path);
    if (auto it = views_.find(path_u8(n.path)); it != views_.end()) editor_.restore_view(it->second.first, it->second.second);
    fade_start_ = anim::clock;
    if (!config && !manual) recent_.visit(n);
    set_title();
}

bool App::need_note() {
    if (is_config()) status("open a note first; the config stays where it is");
    else if (is_manual()) status("open a note first; the manual is read only");
    return is_note();
}

void App::open_manual() {
    NoteRef r;
    r.space = "delta";
    r.name = "manual";
    r.path = kManual;
    open_note(r);
}

void App::close_note() {
    keep_yours();
    save();
    remember_view();
    note_.reset();
    problems_.clear();
    spelling_.clear();
    diag_gen_++;
    editor_.set_text("");
    fade_start_ = anim::clock;
    set_title();
}

void App::switch_space(const std::string& space) {
    if (space == space_ && note_) return;
    if (auto e = store_.ensure_space(space); !e.empty()) { status(e, true); return; }
    auto notes = store_.notes(space);
    if (!notes.empty()) {
        open_note(notes.front());
    } else {
        close_note();
        space_ = space;
    }
}

void App::create_note(const std::string& space, const std::string& raw) {
    std::string name = trim(raw);
    std::string content = cfg_.new_note_heading ? "# " + name + "\n\n" : "";
    NoteRef n;
    if (auto e = store_.create_note(space, name, content, n); !e.empty()) {
        status(e, true);
        palette_.prompt("new note: " + space + "/", raw, "+ new note", [this, space](const std::string& v) { create_note(space, v); });
        return;
    }
    open_note(n);
    editor_.restore_view({(int)editor_.lines().size() - 1, 0}, 0);
    status("created " + name);
}

void App::open_daily() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[128];
    if (!std::strftime(buf, sizeof buf, cfg_.daily_format.c_str(), &tm)) std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    std::string name = buf;
    if (store_.exists(cfg_.daily_space, name)) {
        open_note(store_.ref(cfg_.daily_space, name));
    } else {
        create_note(cfg_.daily_space, name);
    }
}

void App::open_link(const LinkSpan& link) {
    if (link.wiki) { open_by_name(link.target); return; }
    std::string t = trim(link.target);
    if (t.find("://") != std::string::npos || t.rfind("mailto:", 0) == 0) {
        if (!open_url(t)) status("only http, https and mailto links open from notes", true);
        return;
    }
    // Relative markdown links: treat as a note name.
    std::string name;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '%' && i + 2 < t.size()) {
            name += (char)std::strtol(t.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            name += t[i];
        }
    }
    if (name.size() > 3 && lower(name.substr(name.size() - 3)) == ".md") name.resize(name.size() - 3);
    if (name.rfind("./", 0) == 0) name = name.substr(2);
    if (name.rfind("../", 0) == 0) name = name.substr(3);
    open_by_name(name);
}

void App::open_by_name(std::string target) {
    std::string space, name;
    if (!Store::split_target(target, space, name) && space.empty()) return;
    auto find_in = [&](const std::string& sp) -> std::optional<NoteRef> {
        for (auto& n : store_.notes(sp))
            if (iequals(n.name, name)) return n;
        return std::nullopt;
    };
    std::optional<NoteRef> hit;
    if (!space.empty()) {
        for (auto& sp : store_.spaces())
            if (iequals(sp, space)) { hit = find_in(sp); space = sp; }
    } else {
        hit = find_in(space_);
        if (!hit)
            for (auto& sp : store_.spaces())
                if ((hit = find_in(sp))) break;
    }
    if (hit) open_note(*hit);
    else create_note(space.empty() ? space_ : space, name);
}

void App::rename_note() {
    if (!note_) return;
    if (!need_note()) return;
    open_bar(Bar::Rename);  // see draw_rename_bar
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", note_->name.c_str());
    bar_last_ = bar_buf_;
}

void App::move_note() {
    if (!note_) return;
    if (!need_note()) return;
    std::vector<PaletteItem> items;
    for (auto& sp : store_.spaces()) {
        if (sp == note_->space) continue;
        PaletteItem it;
        it.label = sp;
        it.action = [this, sp] {
            save();
            std::string old = path_u8(note_->path);
            NoteRef n = *note_;
            if (auto e = store_.move_note(n, sp); !e.empty()) { status(e, true); return; }
            views_.erase(old);
            auto moved = [&](NoteRef& r) { if (r.path == note_->path) r = n; };
            history_.update(moved);
            recent_.update(moved);
            note_ = n;
            space_ = sp;
            apply_spelling();  // this space may want other dictionaries
            disk_mtime_ = mtime_of(n.path);
            status("moved to " + sp);
        };
        items.push_back(it);
    }
    if (items.empty()) { status("no other space yet; make one with new space"); return; }
    palette_.pick("move to: ", items);
}

void App::delete_note() {
    if (!note_) return;
    if (!need_note()) return;
    NoteRef n = *note_;
    PaletteItem yes, no;
    yes.label = "delete \"" + n.name + "\"";
    yes.meta = "to .trash";
    yes.emphasis = true;
    yes.action = [this, n] {
        save();
        if (auto e = store_.trash_note(n); !e.empty()) { status(e, true); return; }
        views_.erase(path_u8(n.path));
        history_.forget(n.path);
        recent_.forget(n.path);
        note_.reset();
        auto rest = store_.notes(n.space);
        if (!rest.empty()) open_note(rest.front(), false);
        else close_note();
        status("deleted " + n.name + " (it's in .trash)");
    };
    no.label = "keep it";
    no.action = [] {};
    palette_.pick("delete? ", {yes, no});
}

void App::new_space() {
    palette_.prompt("new space: ", "", "+ new space", [this](const std::string& v) {
        std::string name = trim(v);
        if (auto e = store_.ensure_space(name); !e.empty()) { status(e, true); return; }
        switch_space(name);
        status("switched to " + name);
    });
}

void App::rename_space() {
    palette_.prompt("rename space: ", space_, "rename to", [this](const std::string& v) {
        save();
        std::string from = space_, to = trim(v);
        if (auto e = store_.rename_space(from, to); !e.empty()) { status(e, true); return; }
        space_ = to;
        apply_spelling();
        views_.clear();
        auto moved = [&](NoteRef& r) { if (r.space == from) r = store_.ref(to, r.name); };
        history_.update(moved);
        recent_.update(moved);
        if (note_) {
            note_ = store_.ref(to, note_->name);
            disk_mtime_ = mtime_of(note_->path);
        }
        status("renamed space to " + to);
    });
}

void App::delete_space() {
    std::string sp = space_;
    if (store_.note_count(sp) > 0) { status(quote(sp) + " still has notes; move or delete them first", true); return; }
    PaletteItem yes, no;
    yes.label = "delete space \"" + sp + "\"";
    yes.emphasis = true;
    yes.action = [this, sp] {
        close_note();
        if (auto e = store_.delete_space(sp); !e.empty()) { status(e, true); return; }
        std::string next = cfg_.default_space == sp ? "" : cfg_.default_space;
        auto spaces = store_.spaces();
        if (next.empty() && !spaces.empty()) next = spaces.front();
        if (next.empty()) next = "default";
        switch_space(next);
        status("deleted space " + sp);
    };
    no.label = "keep it";
    no.action = [] {};
    palette_.pick("delete space? ", {yes, no});
}

void App::restore_note() {
    std::vector<PaletteItem> items;
    for (auto& t : store_.trashed()) {
        PaletteItem it;
        it.label = t.space + "/" + t.name;
        it.meta = ago(t.mtime);  // when it was deleted
        it.action = [this, t] {
            NoteRef n;
            if (auto e = store_.restore_note(t, n); !e.empty()) { status(e, true); return; }
            open_note(n);
            status("restored " + n.name + (n.name != t.name ? " (its name was taken)" : ""));
        };
        items.push_back(std::move(it));
    }
    if (items.empty()) { status("no deleted notes"); return; }
    palette_.pick("restore: ", std::move(items));
}

// Back and forward, through the places you've jumped between (see History).
void App::history_step(int dir) {
    close_bar();
    const Place* p = history_.step(dir, here(), [](const Place& p) {
        std::error_code ec;
        return p.note.path == kManual || fs::exists(p.note.path, ec);
    });
    if (!p) return;
    Place to = *p;
    if (note_ && note_->path == to.note.path) {
        editor_.return_to(to.caret, to.caret, to.scroll);  // glides there
        return;
    }
    open_note(to.note, false);
    if (note_ && note_->path == to.note.path) editor_.restore_view(to.caret, to.scroll);
}

// What ctrl+tab goes through: the notes you've had open, latest first (so
// the one open now, then the one before). Finding any other note is ctrl+o's.
std::vector<NoteRef> App::switch_list() const {
    std::vector<NoteRef> out;
    std::set<std::string> seen;
    for (auto& n : recent_.list()) {
        std::error_code ec;
        if (!fs::exists(n.path, ec) || !seen.insert(path_u8(n.path)).second) continue;
        out.push_back(store_.ref(n.space, n.name));  // when it was changed, as of now
    }
    return out;
}

// ctrl+tab, like alt+tab: a tap flips to the note you had open before; held,
// the list comes up and tab steps through it until ctrl is let go (see
// frame and Palette::hold).
void App::switch_note(int dir) {
    if (!switch_pending_) {
        switch_notes_ = switch_list();
        int n = (int)switch_notes_.size();
        bool first_open = note_ && n && switch_notes_[0].path == note_->path;
        if (n < (first_open ? 2 : 1)) { status("no other notes yet"); return; }
        switch_pick_ = dir > 0 ? (first_open ? 1 : 0) : n - 1;
        switch_pending_ = true;
        switch_time_ = ImGui::GetTime();
        return;
    }
    // Pressed again before the list showed: show it now, a step on.
    int n = (int)switch_notes_.size();
    switch_pick_ = (switch_pick_ + dir + n) % n;
    switch_time_ = -100;
}

// ---------------------------------------------------------------------------
// Palette

void App::open_palette(const std::string& prefix, bool wide) {
    close_bar();
    cache_notes_ = store_.notes(space_);
    cache_all_.clear();
    cache_spaces_ = store_.spaces();
    cache_text_.clear();
    // Pressing the same shortcut again closes it; the other scope's
    // shortcut switches scope instead.
    if (palette_.in_mode(prefix.empty() ? 0 : prefix[0])) {
        if (palette_.scoped() && palette_.wide() != wide) palette_.set_wide(wide);
        else palette_.close();
        return;
    }
    palette_.open(prefix, wide);
}

// A row for a line with a match in it (search, links here): a window of the
// line around the match, which is highlighted.
static PaletteItem line_item(const std::string& line, size_t at, size_t len) {
    size_t from = at > 40 ? at - 40 : 0;
    while (from > 0 && utf8::is_cont((unsigned char)line[from])) from--;
    std::string snippet = (from ? "..." : "") + line.substr(from, 160);
    int hl0 = (int)(at - from + (from ? 3 : 0));
    PaletteItem it;
    it.label = trim(snippet);
    size_t first = snippet.find_first_not_of(" \t");
    int lead = (int)(snippet.size() - snippet.substr(first == std::string::npos ? 0 : first).size());
    for (int k = 0; k < (int)len; k++) it.hl.push_back(hl0 - lead + k);
    return it;
}

// A note's lines, read once while the palette is open.
const std::vector<std::string>& App::note_lines(const NoteRef& n) {
    auto it = cache_text_.find(path_u8(n.path));
    if (it == cache_text_.end()) {
        std::string text;
        read_file(n.path, text);
        std::vector<std::string> lines(1);
        for (char ch : text) {
            if (ch == '\r') continue;
            if (ch == '\n') lines.emplace_back();
            else lines.back() += ch;
        }
        it = cache_text_.emplace(path_u8(n.path), std::move(lines)).first;
    }
    return it->second;
}

void App::build_palette(const std::string& text, std::vector<PaletteItem>& out, int& total) {
    char c = text.empty() ? 0 : text[0];
    bool prefixed = c && std::strchr(Palette::kModes, c);
    std::string q = trim(prefixed ? text.substr(1) : text);
    auto sort = [&] {
        if (!q.empty()) std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score > b.score; });
    };

    // Notes: this space's, or every space's when widened. "work/plan"
    // narrows to one space from either (and makes the note there).
    if (c == '.') {
        std::string only, name = q;
        if (size_t slash = q.find('/'); slash != std::string::npos) {
            only = trim(q.substr(0, slash));
            name = trim(q.substr(slash + 1));
        }
        bool all = palette_.wide() || !only.empty();
        if (all && cache_all_.empty()) cache_all_ = store_.all_notes();
        std::vector<NoteRef> notes;
        for (auto& n : all ? cache_all_ : cache_notes_)
            if (only.empty() || iequals(n.space, only)) notes.push_back(n);
        // By name (as listed, so every space's are grouped by space), or by
        // recent: the notes you've had open, latest first, then the rest by
        // when they were changed. Matches keep this order among equals.
        if (sort_recent_) {
            std::map<std::string, int> rank;
            for (auto& n : recent_.list()) rank.emplace(path_u8(n.path), (int)rank.size());
            auto rank_of = [&](const NoteRef& n) {
                auto it = rank.find(path_u8(n.path));
                return it == rank.end() ? INT_MAX : it->second;
            };
            std::stable_sort(notes.begin(), notes.end(), [&](const NoteRef& a, const NoteRef& b) {
                int ra = rank_of(a), rb = rank_of(b);
                return ra != rb ? ra < rb : a.mtime > b.mtime;
            });
        } else {
            auto label = [&](const NoteRef& n) { return all ? n.space + "/" + n.name : n.name; };
            std::sort(notes.begin(), notes.end(), [&](const NoteRef& a, const NoteRef& b) { return natural_less(label(a), label(b)); });
        }
        palette_.set_tag(sort_recent_ ? "by recent" : "by name");
        total = (int)notes.size();
        // With nothing typed, by name the open note starts selected (you are
        // here); by recent it's at the top and the one before it is, so
        // enter flips back.
        bool pick_next = sort_recent_ && name.empty();
        for (auto& n : notes) {
            PaletteItem it;
            bool open = note_ && note_->path == n.path;
            std::string where = all ? n.space + "/" : "";
            it.label = where + n.name;
            it.dim = (int)where.size();
            // Past a "space/", only the name is matched.
            if (only.empty()) {
                if (!fuzzy_match(name, it.label, it.score, &it.hl)) continue;
            } else {
                if (!fuzzy_match(name, n.name, it.score, &it.hl)) continue;
                for (int& h : it.hl) h += (int)where.size();
            }
            it.meta = open ? "open" : ago(n.mtime);
            if (pick_next && !open) it.current = true, pick_next = false;
            else if (!sort_recent_ && name.empty() && open) it.current = true;
            it.action = [this, n] { open_note(n); };
            out.push_back(std::move(it));
            if (out.size() > 300) break;
        }
        sort();
        std::string sp = only.empty() ? space_ : only;
        if (!name.empty() && !store_.exists(sp, name)) {
            PaletteItem it;
            it.label = "+ new note \"" + name + "\"";
            it.meta = sp;
            it.emphasis = true;
            it.action = [this, sp, name] { create_note(sp, name); };
            out.push_back(std::move(it));
        }
        return;
    }

    if (c == '/') {
        bool exact = false;
        total = (int)cache_spaces_.size();
        for (auto& sp : cache_spaces_) {
            PaletteItem it;
            it.label = sp;
            if (!fuzzy_match(q, sp, it.score, &it.hl)) continue;
            if (iequals(sp, q)) exact = true;
            int count = store_.note_count(sp);
            it.meta = sp == space_ ? "current" : std::to_string(count) + (count == 1 ? " note" : " notes");
            it.action = [this, sp] { switch_space(sp); };
            out.push_back(std::move(it));
        }
        sort();
        if (!q.empty() && !exact) {
            PaletteItem it;
            it.label = "+ new space \"" + q + "\"";
            it.emphasis = true;
            it.action = [this, q] {
                if (auto e = store_.ensure_space(q); !e.empty()) { status(e, true); return; }
                switch_space(q);
            };
            out.push_back(std::move(it));
        }
        return;
    }

    // The note's outline. With nothing typed, the section you're in starts
    // selected, so up and down step from where you are.
    if (c == '@') {
        if (!note_) { palette_.set_empty("open a note first; headings are listed from the note in view"); return; }
        auto heads = editor_.headings();
        total = (int)heads.size();
        int here = -1;
        for (int k = 0; k < (int)heads.size(); k++)
            if (heads[k].line <= editor_.cursor().line) here = k;
        for (int k = 0; k < (int)heads.size(); k++) {
            const Heading& h = heads[k];
            PaletteItem it;
            it.label = h.text.empty() ? "(untitled)" : h.text;
            if (!fuzzy_match(q, it.label, it.score, &it.hl)) continue;
            it.indent = q.empty() ? h.level - 1 : 0;
            it.meta = std::to_string(h.line + 1);
            it.current = q.empty() && k == here;
            int line = h.line;
            it.action = [this, line] {
                Jump j(*this);
                editor_.goto_line(line);
            };
            out.push_back(std::move(it));
        }
        palette_.set_empty(heads.empty() ? "no headings in this note; start a line with # to add one" : "no heading matches " + quote(q));
        sort();
        return;
    }

    // Line numbers are found in the status line, with the note in view;
    // typing : here hands over to it (see frame).
    if (c == ':') {
        total = -2;
        return;
    }

    if (c == '?') {
        if (q.size() < 2) {
            total = -2;
            PaletteItem it;
            it.label = "at least 2 characters";
            out.push_back(std::move(it));
            return;
        }
        std::string lq = lower(q);
        auto scan = [&](const NoteRef& n, const std::vector<std::string>& lines) {
            for (int i = 0; i < (int)lines.size() && out.size() < 200; i++) {
                std::string ll = lower(lines[i]);
                size_t at = ll.find(lq);
                if (at == std::string::npos) continue;
                PaletteItem it = line_item(lines[i], at, q.size());
                it.meta = (n.space == space_ ? "" : n.space + "/") + n.name + ":" + std::to_string(i + 1);
                TextPos a{i, (int)at}, b{i, (int)(at + q.size())};
                it.action = [this, n, a, b] {
                    Jump j(*this);
                    if (!note_ || note_->path != n.path) open_note(n);
                    editor_.select_range(a, b);
                };
                out.push_back(std::move(it));
            }
        };
        // The open note first, as it is in the editor; then this space's
        // notes, or every space's when widened.
        if (is_note()) scan(*note_, editor_.lines());
        if (palette_.wide() && cache_all_.empty()) cache_all_ = store_.all_notes();
        for (auto& n : palette_.wide() ? cache_all_ : cache_notes_) {
            if (note_ && n.path == note_->path) continue;
            scan(n, note_lines(n));
        }
        palette_.set_empty(palette_.wide() ? "no matches for " + quote(q) + " in any space"
                                           : "no matches for " + quote(q) + " in " + space_ + "; tab searches every space");
        return;
    }

    // Links here: every [[link]] in any space that leads to the open note.
    if (c == '[') {
        if (!is_note()) { palette_.set_empty("open a note first; its links are listed here"); return; }
        if (cache_all_.empty()) cache_all_ = store_.all_notes();
        total = 0;
        for (auto& n : cache_all_) {
            if (n.path == note_->path) continue;
            const auto& lines = note_lines(n);
            for (auto& h : links_to(lines, n.space, note_->space, note_->name, note_index_)) {
                total++;
                PaletteItem it = line_item(lines[h.line], h.col0, h.col1 - h.col0);
                it.meta = (n.space == space_ ? "" : n.space + "/") + n.name + ":" + std::to_string(h.line + 1);
                if (!fuzzy_match(q, it.label + " " + it.meta, it.score, nullptr)) continue;
                TextPos a{h.line, h.col0}, b{h.line, h.col1};
                it.action = [this, n, a, b] {
                    Jump j(*this);
                    open_note(n);
                    editor_.select_range(a, b);
                };
                out.push_back(std::move(it));
            }
        }
        palette_.set_empty(total == 0 ? "no notes link to " + note_->name + " yet; link one with [[" + note_->name + "]]"
                                      : "no links match " + quote(q));
        sort();
        return;
    }

    if (!prefixed) {
        palette_.set_empty("no command matches " + quote(q));
        total = (int)std::count_if(cmds_.begin(), cmds_.end(), [](const Command& c) { return c.in_palette; });
        size_t prefixed_w = 0;
        for (auto& cmd : cmds_)
            if (cmd.prefix && !cmd.chords.empty()) prefixed_w = std::max(prefixed_w, chord_str(cmd.chords.front()).size());
        for (auto& cmd : cmds_) {
            if (!cmd.in_palette) continue;
            PaletteItem it;
            it.label = cmd.title;
            if (!match_command(q, cmd.title, cmd.id, command_keywords(cmd.id), it.score, &it.hl, &it.aside)) continue;
            // Prefixes sit in a column of their own: the palette is mono, so
            // padding the keys to the widest a prefixed command has lines them up.
            std::string keys = cmd.chords.empty() ? "" : chord_str(cmd.chords.front());
            if (cmd.prefix) keys = std::string(1, cmd.prefix) + "  " + std::string(prefixed_w - std::min(prefixed_w, keys.size()), ' ') + keys;
            it.meta = keys;
            auto run = cmd.run;
            it.action = [run] { run(); };
            out.push_back(std::move(it));
        }
        sort();
    }
}

// ---------------------------------------------------------------------------
// Frame

float App::base_size() const { return std::round(cfg_.font_size * kEm * zoom_ * dpi_); }

ImU32 App::background() const { return T.bg; }

void App::status(const std::string& msg, bool error) {
    status_ = msg;
    status_error_ = status_held_ = error;
    status_time_ = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0;
}

void App::on_focus(bool focused) {
    focused_ = focused;
    if (!focused) save();
    else next_poll_ = 0;  // check disk right away
}

double App::next_wake(double now) {
    double t = now + 1.0;
    if (note_ && editing() && cfg_.cursor_blink && focused_ && !palette_.is_open()) t = std::min(t, editor_.next_blink(now));
    if (note_ && !conflict_ && editor_.version() != saved_version_) t = std::min(t, last_change_ + cfg_.autosave_delay);
    double status_end = status_time_ + 3.4;
    if (status_end > now) t = std::min(t, status_end);
    if (note_ && next_problem_check_ > now) t = std::min(t, next_problem_check_);
    if (switch_pending_) t = std::min(t, switch_time_ + 0.15);        // the switcher comes up
    if (spell_pending_ || (bar_ == Bar::Fix && fix_advance_)) t = now;  // more lines to check, or a fix to move on from
    else if (speller_.loading()) t = std::min(t, now + 0.05);        // marks as soon as it's loaded
    if (speller_.suggesting()) t = std::min(t, now + 0.016);         // the echo as soon as its suggestions are in
    if (font_loader_.busy()) t = std::min(t, now + 0.016);           // fonts as soon as they're found
    return t;
}

void App::poll_files(double now) {
    if (now < next_poll_) return;
    next_poll_ = now + 1.0;
    auto m = mtime_of(cfg_path_);
    if (m != cfg_mtime_ && m != fs::file_time_type{}) {
        load_settings();
        if (status_.empty() || !status_error_) status("config reloaded");
    }
    // words.txt edited (here or elsewhere): its words, and no longer those taken out.
    if (auto wm = mtime_of(words_path_); wm != words_mtime_) apply_spelling(true);
    if (!note_) return;
    // Notes made, renamed or removed (here or elsewhere) change which links lead somewhere.
    if (!is_config()) {
        NoteIndex ix = store_.index();
        if (!(ix == note_index_)) {
            note_index_ = std::move(ix);
            hints_version_ = UINT64_MAX;
        }
    }
    auto nm = mtime_of(note_->path);
    if (nm == fs::file_time_type{} || nm == disk_mtime_) return;
    disk_mtime_ = nm;
    if (editor_.version() != saved_version_) conflict_ = true;  // the status line says so until it's settled
    else reload_note();
}

void App::toggle_fullscreen() {
    // Borderless: an undecorated window covering the monitor. A monitor-owned
    // window would be topmost and minimize whenever it loses focus.
    if (!fullscreen_) {
        // Restore first, so the size we come back to is the unmaximized one.
        was_maximized_ = glfwGetWindowAttrib(win_, GLFW_MAXIMIZED);
        if (was_maximized_) glfwRestoreWindow(win_);
        glfwGetWindowPos(win_, &win_x_, &win_y_);
        glfwGetWindowSize(win_, &win_w_, &win_h_);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        // Pick the monitor the window is mostly on.
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        int cx = win_x_ + win_w_ / 2, cy = win_y_ + win_h_ / 2;
        for (int i = 0; i < count; i++) {
            int mx, my;
            glfwGetMonitorPos(mons[i], &mx, &my);
            const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
            if (cx >= mx && cx < mx + vm->width && cy >= my && cy < my + vm->height) mon = mons[i];
        }
        int mx, my;
        glfwGetMonitorPos(mon, &mx, &my);
        const GLFWvidmode* vm = glfwGetVideoMode(mon);
        glfwSetWindowAttrib(win_, GLFW_DECORATED, GLFW_FALSE);
        // One row past the bottom edge: a GL window that exactly matches the
        // monitor gets switched to the driver's fullscreen path, which blanks
        // the screen while the display resyncs.
        glfwSetWindowMonitor(win_, nullptr, mx, my, vm->width, vm->height + 1, 0);
        fullscreen_ = true;
    } else {
        glfwSetWindowAttrib(win_, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowMonitor(win_, nullptr, win_x_, win_y_, win_w_, win_h_, 0);
        if (was_maximized_) glfwMaximizeWindow(win_);
        fullscreen_ = false;
    }
}

void App::dispatch_keys(bool& fired) {
    fired = false;
    if (viewer_.open) return;  // the viewer takes the keys it uses, and nothing else runs
    bool pal = palette_.is_open() || bar_ != Bar::None;  // modal: only global commands
    for (auto& c : cmds_) {
        if (pal && !c.global) continue;
        for (auto& ch : c.chords) {
            if (chord_pressed(ch, c.repeat)) {
                c.run();
                fired = true;
                return;
            }
        }
    }
}

// With no note open, the page shows the ways out, like vim's intro screen.
void App::draw_empty(ImVec2 min, ImVec2 max, float col_x, float column, float top_pad, float page) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = base_size();
    float a = anim::ease_out(anim::progress(fade_start_, anim::clock, 0.14f)) * page;
    struct Row { std::string key, text; };
    std::vector<Row> rows;
    auto add = [&](const char* id, std::string text) {
        std::string k = chord_for(id);
        if (!k.empty()) rows.push_back({k, std::move(text)});
    };
    add("note.new", "new note in " + space_);
    add("note.daily", "today's daily note");
    add("palette.notes", "find a note");
    add("palette.spaces", "switch space");
    add("palette.commands", "all commands");
    add("app.manual", "the manual");

    // Set on the page's lines, spaced like the editor's.
    float row_h = std::round(s * cfg_.line_height);
    float top = min.y + top_pad;
    int lines = std::max(1, (int)((max.y - top) / row_h));

    float ms = std::round(s * cfg_.ui_size);
    auto W = [&](ImFont* f, const std::string& t) { return f->CalcTextSizeA(ms, FLT_MAX, 0, t.c_str()).x; };
    int n = store_.note_count(space_);
    std::string sub = space_ + "/ is empty";
    if (n > 0) sub = space_ + "/ has " + std::to_string(n) + (n == 1 ? " note" : " notes") + ", none open";

    // title, blank, subtitle, blank, then the keys, centred on the page
    int block = 4 + (int)rows.size();
    int first = std::max(0, (int)std::round((lines - block) * 0.42f));
    auto line_y = [&](int i) { return std::round(top + (first + i) * row_h + (row_h - ms) * 0.5f); };
    auto centred = [&](ImFont* f, const std::string& t) { return std::round(col_x + (column - W(f, t)) * 0.5f); };

    dl->AddText(mono_bold_, ms, ImVec2(centred(mono_bold_, "delta"), line_y(0)), col_alpha(T.bright, a), "delta");
    dl->AddText(mono_, ms, ImVec2(centred(mono_, sub), line_y(2)), col_alpha(T.dim, a), sub.c_str());

    float key_w = 0, text_w = 0;
    for (auto& r : rows) {
        key_w = std::max(key_w, W(mono_, r.key));
        text_w = std::max(text_w, W(mono_, r.text));
    }
    float gap = W(mono_, "    ");
    float x = std::round(col_x + (column - key_w - gap - text_w) * 0.5f);
    for (size_t i = 0; i < rows.size(); i++) {
        float y = line_y(4 + (int)i);
        dl->AddText(mono_, ms, ImVec2(x, y), col_alpha(T.text, a), rows[i].key.c_str());
        dl->AddText(mono_, ms, ImVec2(x + key_w + gap, y), col_alpha(T.dim, a), rows[i].text.c_str());
    }
}

// ---------------------------------------------------------------------------
// The status line as a minibuffer (like vim's / or Emacs's C-s): tools that
// act on the note in view (find, go to line, rename) happen down there, so
// the note stays in full view.

void App::open_bar(Bar kind) {
    if (palette_.is_open()) palette_.close();
    if (bar_ != kind) close_bar();
    bar_ = kind;
    // Always start in the first field, its text selected so typing replaces
    // it. Texts can be kept from last time; the focus isn't.
    bar_field_ = 1;
    bar_focus_ = true;
    bar_select_all_ = true;
    bar_error_.clear();
}

void App::close_bar() {
    if (bar_ == Bar::None) return;
    if (bar_ == Bar::Find) editor_.find_end();  // the current match stays selected
    // Where find or go to line took you is a jump (escape from go to line
    // has already gone back, so it isn't).
    if (bar_ == Bar::Find || bar_ == Bar::Line) history_.jumped(bar_from_, here());
    bar_ = Bar::None;
    bar_error_.clear();
}

void App::cancel_bar() {
    if (bar_ == Bar::Line) editor_.return_to(bar_cur_, bar_anchor_, bar_scroll_);
    close_bar();
}

// Keeps a field's text selected (on opening) or the cursor at the end (on
// refocus after Enter), whichever was asked for. The line field takes only
// what a line target can hold.
int App::bar_input_callback(ImGuiInputTextCallbackData* d) {
    auto* app = static_cast<App*>(d->UserData);
    if (d->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        ImWchar c = d->EventChar;
        return (c >= '0' && c <= '9') || c == '+' || c == '-' || c == ':' ? 0 : 1;
    }
    // Text set while the field is being edited (ImGui keeps its own copy).
    if (app->bar_set_pending_) {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, app->bar_set_.c_str());
        app->bar_set_pending_ = false;
    }
    if (app->bar_select_pending_ == 1) { d->SelectionStart = 0; d->SelectionEnd = d->CursorPos = d->BufTextLen; }
    if (app->bar_select_pending_ == 2) { d->SelectionStart = d->SelectionEnd = d->CursorPos = d->BufTextLen; }
    app->bar_select_pending_ = 0;
    return 0;
}

// One text field of the bar. Returns whether it's the one being typed in.
bool App::bar_input(const char* id, char* buf, size_t size, int which, float x, float y, float w) {
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::SetNextItemWidth(w);
    if (bar_focus_ && bar_field_ == which) {
        ImGui::SetKeyboardFocusHere();
        if (bar_select_all_) bar_select_pending_ = 1;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackAlways;
    if (bar_ == Bar::Line) flags |= ImGuiInputTextFlags_CallbackCharFilter;
    // Tab is ours (switching fields), not ImGui's focus cycling.
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);
    ImGui::InputText(id, buf, size, flags, &App::bar_input_callback, this);
    ImGui::PopItemFlag();
    // Track the field you clicked into, but not while a switch we asked for
    // is still landing (the old field is active until next frame).
    bool active = ImGui::IsItemActive();
    if (active && !bar_focus_) bar_field_ = which;
    return active;
}

void App::draw_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { cancel_bar(); return; }
    // Fields in the status line's mono type.
    ImGui::PushFont(mono_, s);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.bright));
    ImGui::GetStyle().InputTextCursorSize = std::max(1.0f, std::round(2 * dpi_));
    switch (bar_) {
        case Bar::Find: draw_find_bar(min, max, s, pad, ty); break;
        case Bar::Line: draw_line_bar(min, max, s, pad, ty); break;
        case Bar::Rename: draw_rename_bar(min, max, s, pad, ty); break;
        case Bar::Fix: draw_fix_bar(min, max, s, pad, ty); break;
        case Bar::None: break;
    }
    bar_focus_ = false;
    bar_select_all_ = false;
    // Keep typing going to the bar: refocus if nothing's active (Enter ends
    // a field's editing in ImGui).
    if (bar_ != Bar::None && !ImGui::IsAnyItemActive()) {
        bar_focus_ = true;
        if (!bar_select_pending_) bar_select_pending_ = 2;
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// Find in this note.

void App::open_find(bool replace) {
    if (!note_) return;
    if (editor_.reader()) editor_.set_reader(false);
    if (bar_ != Bar::Find) {
        // A selection on one line becomes the query.
        if (editor_.has_selection() && editor_.cursor().line == editor_.anchor().line)
            std::snprintf(find_buf_, sizeof find_buf_, "%s", editor_.selected_text().c_str());
        open_bar(Bar::Find);
        bar_from_ = here();
        editor_.find_begin();
        find_last_ = "\x01";  // match again on the next frame
    } else {
        open_bar(Bar::Find);
    }
    find_replace_ = replace;  // ctrl+f: find alone; ctrl+h: find and replace
}

void App::draw_find_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const char* t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t).x; };
    ImGuiIO& io = ImGui::GetIO();

    // Keys first, against the field that was active last frame.
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (enter && io.KeyCtrl && io.KeyAlt && find_replace_) {
        int n = editor_.replace_all(repl_buf_);
        status(n ? "replaced " + std::to_string(n) : "nothing to replace");
        find_last_ = find_buf_;
    } else if (enter && bar_field_ == 2) {
        editor_.replace_current(repl_buf_);
        find_last_ = find_buf_;
    } else if (enter) {
        editor_.find_step(io.KeyShift ? -1 : 1);
    }
    if (enter) { bar_focus_ = true; bar_select_pending_ = 2; }
    if (find_replace_ && ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        bar_field_ = bar_field_ == 1 ? 2 : 1;
        bar_focus_ = true;
        bar_select_pending_ = 1;
    }

    float x = min.x + pad;
    float field_w = std::min(24 * s, (max.x - min.x) * 0.3f);
    auto field = [&](const char* label, const char* id, char* buf, size_t size, int which) {
        dl->AddText(mono_, s, ImVec2(x, ty), T.dim, label);
        x += W(label) + s * 0.5f;
        bar_input(id, buf, size, which, x, ty, field_w);
        x += field_w + s * 1.6f;
    };
    field("find", "##find", find_buf_, sizeof find_buf_, 1);
    if (find_replace_) field("replace", "##replace", repl_buf_, sizeof repl_buf_, 2);

    // Match as you type.
    if (find_last_ != find_buf_) {
        find_last_ = find_buf_;
        editor_.find_set(find_buf_);
    }

    // Right: where you are among the matches.
    std::string count;
    if (find_buf_[0]) {
        int n = editor_.find_count();
        count = n ? std::to_string(editor_.find_index() + 1) + "/" + std::to_string(n) : "no matches";
    }
    if (!count.empty()) {
        float w = W(count.c_str());
        dl->AddText(mono_, s, ImVec2(max.x - pad - w, ty), editor_.find_count() ? T.dim : T.text, count.c_str());
    }
}

// Fix spelling (see fix_spelling).

void App::draw_fix_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };
    ImGuiIO& io = ImGui::GetIO();
    const auto& lines = editor_.lines();
    float x = min.x + pad;
    dl->AddText(mono_, s, ImVec2(x, ty), T.dim, "fix");
    x += W("fix") + s * 0.5f;

    // After a fix, a skip or an add: the next word, once the spelling marks
    // are current. Forward to the end, then from the top back to where the
    // pass began.
    if (fix_advance_) {
        if (!speller_.ready() || spell_version_ != editor_.version() || spell_gen_ != speller_.generation() || spell_pending_) return;
        fix_advance_ = false;
        int next = fix_pass_next(spelling_, fix_from_, fix_origin_, fix_wrapped_);
        if (next < 0) {
            close_bar();
            status(spelling_.empty() ? "no misspelt words left" : "back where you started");
            return;
        }
        fix_load(spelling_[next]);
    }

    // The word must still be where it was (an undo, a reload...).
    if (fix_.line >= (int)lines.size() || fix_.col1 > (int)lines[fix_.line].size() ||
        lines[fix_.line].compare(fix_.col0, fix_.col1 - fix_.col0, fix_.word) != 0) {
        close_bar();
        return;
    }

    // What the keys (and clicks) do.
    int n = (int)fix_.sugg.size();
    auto fix_with = [&](const std::string& v) {
        TextPos after{fix_.line, fix_.col1};
        if (!v.empty() && v != fix_.word) {
            editor_.replace_range({fix_.line, fix_.col0}, {fix_.line, fix_.col1}, v);
            // The pass goes on after the fix; where it began moves with the text.
            int old_len = (int)fix_.word.size(), new_len = (int)v.size();
            after = after_replace(after, fix_.line, fix_.col0, old_len, new_len);
            fix_origin_ = after_replace(fix_origin_, fix_.line, fix_.col0, old_len, new_len);
        }
        fix_next(after);
    };
    auto skip = [&] { fix_next({fix_.line, fix_.col1}); };
    auto add = [&] {
        add_to_dictionary(spell_form(lines[fix_.line], {fix_.col0, fix_.col1}));
        fix_next({fix_.line, fix_.col1});
    };
    auto guess = [&](int dir) {
        fix_.pick = (fix_.pick + (dir > 0 ? 1 : n - 1)) % n;
        fix_field(fix_.sugg[fix_.pick]);
    };

    // No key repeat: holding enter mustn't race through fixes.
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
    if (enter) {
        if (io.KeyCtrl) add();
        else if (io.KeyShift) skip();
        else fix_with(trim(bar_buf_));
        return;
    }
    if (n && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) guess(io.KeyShift ? -1 : 1);

    // Anything that names an action can be clicked: it lights up under the
    // pointer, and the click runs once the bar is drawn.
    std::function<void()> clicked;
    auto hot = [&](float x0, float x1) {
        ImVec2 m = io.MousePos;
        bool over = m.x >= x0 && m.x < x1 && m.y >= min.y && m.y < max.y;
        if (over) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return 2;
        }
        return over ? 1 : 0;
    };

    // Left: the field, wide enough for the longest suggestion.
    float widest = W(fix_.word);
    for (auto& g : fix_.sugg) widest = std::max(widest, W(g));
    float field_w = std::clamp(widest + s * 2, 10 * s, (max.x - min.x) * 0.3f);
    bar_input("##fix", bar_buf_, sizeof bar_buf_, 1, x, ty, field_w);
    x += field_w + s * 1.6f;

    // Right: which suggestion is in the field, and the keys (while there's room).
    float rx = max.x - pad;
    std::string v = trim(bar_buf_);
    std::string count = !n ? "no suggestions" : v == fix_.sugg[fix_.pick] ? std::to_string(fix_.pick + 1) + "/" + std::to_string(n) : "your own";
    const char* sep = " \xC2\xB7 ";  // " · "
    std::vector<std::pair<std::string, std::function<void()>>> keys;
    if (n) keys.push_back({"tab next", [&] { guess(1); }});
    keys.push_back({"shift+enter skip", skip});
    keys.push_back({"ctrl+enter add", add});
    float keys_w = 0;
    for (auto& [label, act] : keys) keys_w += W(label) + (keys_w > 0 ? W(sep) : 0);
    if (rx - keys_w - s * 1.6f - W(count) > x + s * 8) {
        float kx = rx - keys_w;
        ImU32 quiet = T.dim;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) {
                dl->AddText(mono_, s, ImVec2(kx, ty), quiet, sep);
                kx += W(sep);
            }
            int h = hot(kx, kx + W(keys[i].first));
            if (h == 2) clicked = keys[i].second;
            dl->AddText(mono_, s, ImVec2(kx, ty), h ? T.text : quiet, keys[i].first.c_str());
            kx += W(keys[i].first);
        }
        rx -= keys_w + s * 1.6f;
    }
    rx -= W(count);
    dl->AddText(mono_, s, ImVec2(rx, ty), T.dim, count.c_str());
    rx -= s * 1.6f;

    // Middle: all the suggestions, the one in the field brighter. A click
    // fixes the word with it, as enter would.
    dl->PushClipRect(ImVec2(x, min.y), ImVec2(std::max(x, rx), max.y), true);
    for (int i = 0; i < n; i++) {
        if (i) {
            dl->AddText(mono_, s, ImVec2(x, ty), T.faint, sep);
            x += W(sep);
        }
        const std::string& g = fix_.sugg[i];
        int h = x < rx ? hot(x, std::min(x + W(g), rx)) : 0;
        if (h == 2) clicked = [&fix_with, g] { fix_with(g); };
        bool in_field = i == fix_.pick && v == g;
        dl->AddText(mono_, s, ImVec2(x, ty), h ? T.bright : in_field ? T.text : T.dim, g.c_str());
        x += W(g);
    }
    dl->PopClipRect();

    if (clicked) clicked();
}

// Go to line: "42", "42:10" (line and column) or "+5" / "-5" from where you
// were. The page follows as you type; escape goes back.

static bool parse_line_target(const std::string& q, int origin, const std::vector<std::string>& lines, TextPos& to) {
    if (q.empty()) return false;
    size_t i = 0;
    int sign = 0;
    if (q[0] == '+' || q[0] == '-') { sign = q[0] == '+' ? 1 : -1; i = 1; }
    auto number = [&](long& n) {
        size_t start = i;
        n = 0;
        while (i < q.size() && q[i] >= '0' && q[i] <= '9' && n < 10000000) n = n * 10 + (q[i++] - '0');
        return i > start;
    };
    long line = 0, col = 1;
    if (!number(line)) return false;
    if (i < q.size() && q[i] == ':') {
        i++;
        if (i < q.size() && !number(col)) return false;
    }
    if (i != q.size()) return false;
    long target = sign ? origin + sign * line : line - 1;
    if (!sign && line < 1) return false;
    to.line = (int)std::clamp<long>(target, 0, (long)lines.size() - 1);
    // Columns count characters, like the status bar's.
    const std::string& s = lines[to.line];
    int b = 0;
    for (long c = 1; c < col && b < (int)s.size(); c++) b = utf8::next(s, b);
    to.col = b;
    return true;
}

void App::open_line(const std::string& text) {
    if (!note_) return;
    bool fresh = bar_ != Bar::Line;
    if (fresh) {
        bar_cur_ = editor_.cursor();
        bar_anchor_ = editor_.anchor();
        bar_scroll_ = editor_.scroll();
    }
    open_bar(Bar::Line);
    if (fresh) bar_from_ = here();
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", text.c_str());
    bar_last_.clear();
    if (!text.empty()) { bar_select_all_ = false; bar_select_pending_ = 2; }
}

void App::draw_line_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };
    const auto& lines = editor_.lines();
    std::string q = trim(bar_buf_);
    TextPos to;
    bool ok = parse_line_target(q, bar_cur_.line, lines, to);
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (enter && (ok || q.empty())) { close_bar(); return; }

    float x = min.x + pad;
    dl->AddText(mono_, s, ImVec2(x, ty), T.dim, "line");
    x += W("line") + s * 0.5f;
    bar_input("##line", bar_buf_, sizeof bar_buf_, 1, x, ty, std::min(12 * s, (max.x - min.x) * 0.3f));

    // The page follows as you type.
    q = trim(bar_buf_);
    ok = parse_line_target(q, bar_cur_.line, lines, to);
    if (q != bar_last_) {
        bar_last_ = q;
        if (ok) editor_.jump_to(to);
        else if (q.empty()) editor_.return_to(bar_cur_, bar_anchor_, bar_scroll_);
    }
    std::string n = std::to_string(lines.size());
    std::string right = q.empty() ? "1 to " + n + ", or +/- lines" : ok ? "of " + n : "a line, 1 to " + n;
    dl->AddText(mono_, s, ImVec2(max.x - pad - W(right), ty), q.empty() || ok ? T.dim : T.text, right.c_str());
}

// Rename: the note's name in the status line becomes editable where it sits.

void App::draw_rename_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };
    if (!note_) { close_bar(); return; }
    std::string v = trim(bar_buf_);
    auto problem = [&]() -> std::string {
        if (v == note_->name) return {};
        if (auto e = Store::check_name(v, "note"); !e.empty()) return e;
        if (store_.exists(note_->space, v) && !iequals(v, note_->name)) return "there's already a note called " + v;
        return {};
    };

    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (enter) {
        if (v == note_->name) { close_bar(); return; }
        std::string e = problem();
        if (e.empty()) {
            save();
            std::string old = path_u8(note_->path);
            NoteRef n = *note_;
            e = store_.rename_note(n, v);
            if (e.empty()) {
                views_.erase(old);
                auto renamed = [&](NoteRef& r) { if (path_u8(r.path) == old) r = n; };
                history_.update(renamed);
                recent_.update(renamed);
                note_ = n;
                disk_mtime_ = mtime_of(n.path);
                set_title();
                close_bar();
                status("renamed to " + n.name);
                return;
            }
        }
        bar_error_ = e;
        bar_focus_ = true;
        bar_select_pending_ = 2;
    }

    // Where the name sits: after the space, as in the normal status line.
    float x = min.x + pad;
    dl->AddText(mono_, s, ImVec2(x, ty), T.dim, note_->space.c_str());
    x += W(note_->space);
    dl->AddText(mono_, s, ImVec2(x, ty), T.faint, " / ");
    x += W(" / ");
    float w = std::clamp(W(bar_buf_) + s * 2, s * 12, (max.x - min.x) * 0.45f);
    bar_input("##rename", bar_buf_, sizeof bar_buf_, 1, x, ty, w);

    if (bar_last_ != bar_buf_) { bar_last_ = bar_buf_; bar_error_.clear(); }
    v = trim(bar_buf_);
    std::string msg = bar_error_.empty() ? problem() : bar_error_;
    std::string right = msg.empty() ? "enter to rename" : msg;
    float rx = std::max(x + w + s * 1.6f, max.x - pad - W(right));
    dl->PushClipRect(ImVec2(x + w, min.y), ImVec2(max.x - pad, max.y), true);
    dl->AddText(mono_, s, ImVec2(rx, ty), msg.empty() ? T.dim : T.text, right.c_str());
    dl->PopClipRect();
}

void App::draw_status(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = std::round(base_size() * cfg_.ui_size);
    float pad = std::round(16 * dpi_);
    float ty = std::round(min.y + (max.y - min.y - s) * 0.5f);
    if (bar_ != Bar::None) { draw_bar(min, max, s, pad, ty); return; }
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };

    // Left: where you are.
    float x = min.x + pad;
    const std::string& where = note_ ? note_->space : space_;  // "config" for delta.ini
    dl->AddText(mono_, s, ImVec2(x, ty), T.dim, where.c_str());
    x += W(where);
    if (note_) {
        dl->AddText(mono_, s, ImVec2(x, ty), T.faint, " / ");
        x += W(" / ");
        // The note name is the "you are here", so it gets the weight.
        dl->AddText(mono_bold_, s, ImVec2(x, ty), T.text, note_->name.c_str());
        x += mono_bold_->CalcTextSizeA(s, FLT_MAX, 0, note_->name.c_str()).x;
        if (editor_.version() != saved_version_) {
            float r = std::max(2.0f, std::round(s * 0.16f));
            dl->AddCircleFilled(ImVec2(x + s * 0.7f, ty + s * 0.55f), r, T.accent, 12);
        }
    }
    float left_end = x + s * 2;

    // Right: counts and mode.
    float rx = max.x - pad;
    auto put_right = [&](const std::string& t, ImU32 col) {
        rx -= W(t);
        dl->AddText(mono_, s, ImVec2(rx, ty), col, t.c_str());
        rx -= s * 1.6f;
    };
    if (note_) {
        put_right(editor_.reader() ? "read" : "edit", editor_.reader() ? T.text : T.dim);
        if (!editor_.reader()) {
            TextPos c = editor_.cursor();
            const std::string& line = editor_.lines()[c.line];
            int col = 1;
            for (int i = 0; i < c.col && i < (int)line.size(); i++)
                if (!utf8::is_cont((unsigned char)line[i])) col++;
            put_right(std::to_string(c.line + 1) + ":" + std::to_string(col), T.dim);
        }
        // The problem count is a mark like the ones on the lines; click it
        // (or press F8) to go to the next one. A note only shows it when
        // there's something to count.
        // Misspelt words are counted the same way, apart (alt+F8).
        auto count = [&](const std::string& t, bool live, ImU32 col, const std::function<void()>& go) {
            float right = rx;
            put_right(t, live ? col : T.dim);
            ImVec2 mouse = ImGui::GetIO().MousePos;
            bool over = live && mouse.x >= right - W(t) && mouse.x < right && mouse.y >= min.y && mouse.y < max.y;
            if (over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !palette_.visible() && !viewer_.open) go();
        };
        int n = (int)problems_.size();
        if (n || is_config())
            count(n == 0 ? "no problems" : std::to_string(n) + (n == 1 ? " problem" : " problems"), n > 0, T.error, [this] { goto_problem(1); });
        int m = (int)spelling_.size() - (shown_held_.line >= 0 ? 1 : 0);  // not the word being typed
        if (m && !is_config())
            count(std::to_string(m) + (m == 1 ? " misspelling" : " misspellings"), true, T.spelling, [this] { goto_misspelling(1); });
        if (!is_config()) {
            int w = editor_.word_count();
            put_right(std::to_string(w) + (w == 1 ? " word" : " words"), T.dim);
        }
        // (Not for a picked image: its markdown's length means nothing.)
        if (editor_.has_selection() && editor_.image_for_caret() < 0) put_right(std::to_string(editor_.selection_length()) + " selected", T.dim);
    }

    // A message too long for the room ends in an ellipsis, not mid-letter.
    auto fit = [&](std::string t) {
        float room = rx - left_end;
        if (W(t) <= room) return t;
        const std::string dots = "\xE2\x80\xA6";  // "…"
        while (!t.empty() && W(t + dots) > room) {
            size_t n = t.size() - 1;
            while (n > 0 && utf8::is_cont((unsigned char)t[n])) n--;
            t.resize(n);
        }
        return t + dots;
    };
    auto centred = [&](const std::string& t, ImU32 col) {
        // Centred on the bar, unless that would push it past the counts.
        float mx = std::max(left_end, std::min(std::round((min.x + max.x - W(t)) * 0.5f), rx - W(t)));
        dl->PushClipRect(ImVec2(left_end, min.y), ImVec2(rx, max.y), true);
        dl->AddText(mono_, s, ImVec2(mx, ty), col, t.c_str());
        dl->PopClipRect();
    };

    // Transient message, between the two. An error stays until the next key.
    double now = ImGui::GetTime();
    float age = (float)(now - status_time_);
    if (status_held_) age = std::min(age, 3.0f);
    if (!status_.empty() && age < 3.4f) {
        float a = age < 3.0f ? 1.0f : 1.0f - (age - 3.0f) / 0.4f;
        float in = anim::ease_out(std::min(1.0f, age / 0.1f));
        if (age < 0.1f) anim::active = true;
        if (age >= 3.0f && !status_held_) anim::active = true;
        centred(fit(status_), col_alpha(status_error_ ? T.bright : T.text, a * in));
    } else if (age >= 3.4f) {
        status_.clear();
    }

    // Otherwise, while the note on disk and yours differ, the two ways out.
    if (status_.empty() && conflict_) {
        std::string save = chord_for("note.save");
        centred(fit("changed on disk; " + (save.empty() ? "\"save note now\"" : save) +
                    " keeps your version, \"reload note\" takes the file's"),
                T.bright);
    }

    // Otherwise, in the same place, the echo of what the caret is on.
    if (!echo_.empty()) echo_shown_ = echo_;
    echo_a_ = anim::approach(echo_a_, echo_.empty() ? 0.0f : 1.0f, 16.0f, ImGui::GetIO().DeltaTime, 0.004f);
    if (status_.empty() && !conflict_ && echo_a_ > 0.0f && !echo_shown_.empty()) {
        // Centred in the room between the note's name and the counts.
        float w = 0;
        for (auto& e : echo_shown_) w += W(e.text);
        float ex = std::max(left_end, std::round((left_end + rx - w) * 0.5f));
        // The fix and the keys can be clicked: they light up under the pointer.
        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool can_click = !echo_.empty() && !palette_.visible() && !viewer_.open;
        Echo::Act act = Echo::Act::None;
        dl->PushClipRect(ImVec2(left_end, min.y), ImVec2(rx, max.y), true);
        for (auto& e : echo_shown_) {
            float x1 = std::min(ex + W(e.text), rx);
            bool over = can_click && e.act != Echo::Act::None && mouse.x >= ex && mouse.x < x1 && mouse.y >= min.y && mouse.y < max.y;
            if (over) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) act = e.act;
            }
            ImU32 col = e.tone == Tone::Strong ? T.text : e.tone == Tone::Quiet ? T.dim : col_mix(T.dim, T.text, 0.4f);
            if (over) col = e.tone == Tone::Strong ? T.bright : T.text;
            dl->AddText(mono_, s, ImVec2(ex, ty), col_alpha(col, echo_a_), e.text.c_str());
            ex += W(e.text);
        }
        dl->PopClipRect();
        if (act == Echo::Act::QuickFix) quick_fix();
        if (act == Echo::Act::FixSpelling) fix_spelling();
    }
}

void App::frame() {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    images_.frame(now);
    poll_downloads();
    handle_drops();

    // A held error starts to fade at the first key after it (not the one
    // that caused it).
    if (status_held_ && now > status_time_)
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            if (ImGui::IsKeyPressed((ImGuiKey)k, false)) {
                status_held_ = false;
                status_time_ = now - 3.0;
                break;
            }

    // Commands passed on the command line (used for testing and scripting).
    // A scripted key press (see "key:" below) is released the frame after.
    if (release_key_ != ImGuiKey_None) {
        ImGuiIO& kio = ImGui::GetIO();
        kio.AddKeyEvent(release_key_, false);
        for (ImGuiKey m : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt}) kio.AddKeyEvent(m, false);
        release_key_ = ImGuiKey_None;
    }
    // "wait:<seconds>" pauses the list, so later steps run once the page is up.
    if (!pending_run_.empty() && now >= run_after_) {
        auto run = std::move(pending_run_);
        pending_run_.clear();
        for (size_t k = 0; k < run.size(); k++) {
            const std::string& id = run[k];
            if (id.rfind("wait:", 0) == 0) {
                run_after_ = now + std::atof(id.c_str() + 5);
                pending_run_.assign(run.begin() + k + 1, run.end());
                break;
            }
            if (id.rfind("open:", 0) == 0) { open_by_name(id.substr(5)); continue; }
            // "drop:<path>" drops a file at the caret; "fetch:<url>" pastes
            // an address as if it were on the clipboard.
            if (id.rfind("drop:", 0) == 0) { on_drop({id.substr(5)}, ImVec2(-1, -1)); continue; }
            if (id.rfind("fetch:", 0) == 0) { if (editing()) fetch_image(id.substr(6)); continue; }
            if (id.rfind("palette:", 0) == 0) { open_palette(id.substr(8)); continue; }
            if (id.rfind("find:", 0) == 0 || id.rfind("replace:", 0) == 0) {
                bool rep = id[0] == 'r';
                open_find(rep);
                std::snprintf(find_buf_, sizeof find_buf_, "%s", id.c_str() + (rep ? 8 : 5));
                continue;
            }
            // "key:<chord>" presses a key for a frame (then the list pauses a
            // moment so it's released before the next step); "type:<text>"
            // types text. For scripting UI checks.
            if (id.rfind("key:", 0) == 0) {
                Chord c;
                if (parse_chord(id.substr(4), c)) {
                    ImGuiIO& kio = ImGui::GetIO();
                    for (ImGuiKey m : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt}) kio.AddKeyEvent(m, (c.mods & m) != 0);
                    kio.AddKeyEvent(c.key, true);
                    release_key_ = c.key;
                }
                run_after_ = now + 0.1;
                pending_run_.assign(run.begin() + k + 1, run.end());
                break;
            }
            // "mouse:<x>:<y>" moves the pointer (window pixels); "down" and
            // "up" press and let go of the left button, "rdown" and "rup"
            // the right.
            if (id.rfind("mouse:", 0) == 0 || id == "down" || id == "up" || id == "rdown" || id == "rup") {
                ImGuiIO& mio = ImGui::GetIO();
                float mx = 0, my = 0;
                if (id[0] == 'm' && std::sscanf(id.c_str() + 6, "%f:%f", &mx, &my) == 2) mio.AddMousePosEvent(mx, my);
                else if (id[0] == 'r') mio.AddMouseButtonEvent(ImGuiMouseButton_Right, id == "rdown");
                else if (id[0] != 'm') mio.AddMouseButtonEvent(ImGuiMouseButton_Left, id == "down");
                run_after_ = now + 0.05;
                pending_run_.assign(run.begin() + k + 1, run.end());
                break;
            }
            if (id.rfind("type:", 0) == 0) {
                ImGui::GetIO().AddInputCharactersUTF8(id.c_str() + 5);
                run_after_ = now + 0.1;
                pending_run_.assign(run.begin() + k + 1, run.end());
                break;
            }
            if (Command* c = find_command(id)) c->run();
        }
    }

    poll_files(now);

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);

    bool fired = false;
    dispatch_keys(fired);

    // The mouse's back and forward buttons (4 and 5), as in a browser.
    for (int b : {3, 4}) {
        if (viewer_.open || !ImGui::IsMouseClicked(b)) continue;
        if (palette_.is_open()) palette_.close();
        history_step(b == 3 ? -1 : 1);
    }

    // ctrl+tab: let go of ctrl straight away and it flips; hold it and the
    // list comes up (see switch_note).
    if (switch_pending_) {
        if (!io.KeyCtrl) {
            switch_pending_ = false;
            open_note(switch_notes_[switch_pick_]);
        } else if (now - switch_time_ >= 0.15) {
            switch_pending_ = false;
            std::vector<PaletteItem> items;
            for (auto& n : switch_notes_) {
                PaletteItem it;
                it.label = n.space == space_ ? n.name : n.space + "/" + n.name;
                it.meta = note_ && n.path == note_->path ? "open" : ago(n.mtime);
                it.action = [this, n] { open_note(n); };
                items.push_back(std::move(it));
            }
            close_bar();
            palette_.hold("switch to: ", std::move(items), switch_pick_);
        }
    }

    float s = base_size();
    float bar_h = std::round(s * std::max(cfg_.ui_size, 0.86f) * 1.75f);  // room for its text
    ImVec2 min = vp->Pos, max(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y - bar_h);

    // The text column, shared by the editor and the palette (which is set in it).
    float ch = sans_->GetFontBaked(s)->GetCharAdvance('0');
    float margin = std::round(28 * dpi_);
    float column = std::round(std::min(cfg_.column_width * ch, (max.x - min.x) - margin * 2));
    float col_x = std::round(min.x + (max.x - min.x - column) * 0.5f);
    float top_pad = std::round(std::max(28 * dpi_, (max.y - min.y) * 0.06f));

    EditorParams::Caret caret = cfg_.caret_style == "bar" ? EditorParams::Caret::Bar
                              : cfg_.caret_style == "block" ? EditorParams::Caret::Block
                                                             : EditorParams::Caret::Thick;

    // While the palette is up, the page steps aside entirely.
    float page = palette_.page_alpha();
    float fade = anim::ease_out(anim::progress(fade_start_, anim::clock, 0.14f));
    if (note_) {
        EditorParams p;
        p.sans = sans_;
        p.mono = mono_;
        p.sans_bold = sans_bold_ != sans_ ? sans_bold_ : nullptr;  // none: drawn doubled
        p.mono_bold = mono_bold_ != mono_ ? mono_bold_ : nullptr;
        p.size = s;
        p.line_height = cfg_.line_height;
        p.code_size = cfg_.code_size;
        p.small_size = cfg_.small_size;
        p.ui_size = cfg_.ui_size;
        p.column = column;
        p.min = min;
        p.max = max;
        p.top_pad = top_pad;
        p.dpi = dpi_;
        p.tab_size = cfg_.tab_size;
        p.line_numbers = cfg_.line_numbers;
        p.blink = cfg_.cursor_blink && focused_;
        p.caret = caret;
        p.caret_trail = cfg_.caret_trail;
        p.fade_syntax = cfg_.fade_syntax;
        p.images = &images_;
        p.px = dpi_ * zoom_;  // image sizes (|300) are CSS px, like the font's
        p.blank_line = cfg_.blank_line_height;
        p.scroll_past_end = cfg_.scroll_past_end;
        p.keyboard = !palette_.is_open() && bar_ == Bar::None && !fired && !viewer_.open;
        p.mouse = !palette_.visible() && !viewer_.open;
        // Space on a picked image opens it (no text goes there anyway).
        if (p.keyboard && editing() && editor_.image_for_caret() >= 0 &&
            !io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            int tile = 0;
            int line = editor_.image_for_caret(&tile);
            open_viewer(line, tile);
            io.InputQueueCharacters.resize(0);
            p.keyboard = p.mouse = false;
        }
        p.alpha = fade * page;
        if (fired) io.InputQueueCharacters.resize(0);
        if (editor_.version() != hints_version_) {
            hints_version_ = editor_.version();
            if (is_config()) {
                editor_.set_hints(config_hints(editor_.lines(), cfg_.theme, binding_docs()));
                // Unsaved edits are checked as they're typed; once saved,
                // load_settings has the file's own verdict.
                if (editor_.version() != saved_version_) {
                    Config live;
                    std::vector<ConfigIssue> issues;
                    parse_config(editor_.text(), live, issues);
                    check_settings(live, issues);
                    set_problems(std::move(issues));
                }
            } else {
                refresh_problems();
            }
        }
        check_spelling();
        show_problems(now);
        update_echo();
        editor_.draw(p);
        // Double-click an image to view it (in the reader, a click will do).
        int img_line = -1, img_tile = 0;
        if (p.mouse && !editor_.over_image_handle() && editor_.image_at(io.MousePos, img_line, img_tile) &&
            (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || (editor_.reader() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
            open_viewer(img_line, img_tile);
        // Right-click a misspelt word to fix it: the status-line fix tool,
        // on that word (where other apps put their suggestions menu).
        if (p.mouse && editing() && !is_config() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            check_spelling();  // this frame's typing may have moved things
            TextPos at = editor_.position_at(io.MousePos);
            for (auto& d : spelling_)
                if (d.line == at.line && d.col0 <= at.col && at.col <= d.col1) {
                    editor_.jump_to({d.line, d.col0});
                    fix_spelling();
                    break;
                }
        }
    } else {
        draw_empty(min, max, col_x, column, top_pad, page);
    }
    draw_status(ImVec2(min.x, max.y), ImVec2(max.x, max.y + bar_h));
    // Clicking into the note ends the bar's tool, keeping where it left you
    // (the click places the caret anyway). A question is left unanswered.
    if (bar_ != Bar::None && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.y < max.y) close_bar();
    ImGui::End();

    // Palette, set in the same column where the page's first line sits.
    // The palette's caret matches the page's (a block makes no sense in a
    // one-line input, so it gets the thick bar).
    float palette_caret = caret_width(caret == EditorParams::Caret::Block ? EditorParams::Caret::Thick : caret, s, dpi_);
    Palette::Frame pf{mono_, mono_bold_, s, std::round(s * cfg_.ui_size), dpi_, col_x, column, min.y + top_pad, max.y - top_pad, now, io.DeltaTime,
                      cfg_.palette_selection == "invert" ? Palette::Selection::Invert : Palette::Selection::Marker,
                      palette_caret, cfg_.cursor_blink && focused_};
    // The prompt names where you are, like a minibuffer.
    auto prompt_for = [this](char mode) -> std::string {
        switch (mode) {
            case '/': return "spaces: ";
            case '@': return "headings: ";
            case ':': return "line: ";
            case '?': return "search " + (palette_.wide() ? std::string("all") : space_) + ": ";
            case '[': return note_ ? "links to " + note_->name + ": " : "links here: ";
            case '.':  // a query naming its space ("work/plan") is the whole path
                if (palette_.text().find('/') != std::string::npos) return "";
                return palette_.wide() ? "all/" : space_ + "/";
            default: return "commands: ";
        }
    };
    auto chosen = palette_.draw(
        pf, [this](const std::string& t, std::vector<PaletteItem>& out, int& total) { build_palette(t, out, total); }, prompt_for);
    if (chosen) chosen();
    // The font picker's preview font goes with it. One wanted is asked for
    // (and, once in, loaded) between frames, so there have to be some.
    if (!font_picker_open()) {
        font_picker_ = 0;
        preview_want_.clear();
    }
    if (preview_want_ != preview_loaded_) anim::active = true;
    // The theme picker left without a pick: the config's theme again.
    if (!theme_picker_open() && theme_shown_ != cfg_.theme) show_theme(cfg_.theme);
    draw_viewer();
    if (palette_.is_open() && note_) {
        std::string t = palette_.text();
        if (!t.empty() && t[0] == ':') {
            palette_.close();
            open_line(t.substr(1));
        }
    }

    // Autosave after a short idle.
    if (note_) {
        if (editor_.version() != seen_version_) {
            seen_version_ = editor_.version();
            last_change_ = now;
        }
        if (!conflict_ && editor_.version() != saved_version_ && now - last_change_ >= cfg_.autosave_delay) save();
    }
}

void App::shutdown() {
    keep_yours();
    save();
    images_.shutdown();  // while the GL context is still around
    std::map<std::string, std::string> kv;
    kv["space"] = space_;
    kv["note"] = is_note() ? note_->name : "";
    kv["zoom"] = std::to_string(zoom_);
    kv["reader"] = (is_note() ? editor_.reader() : reader_pref_) ? "1" : "0";
    kv["sort"] = sort_recent_ ? "recent" : "name";
    // Where each note's caret and scroll were, so notes reopen where you left
    // them. Paths are relative to the notes folder.
    remember_view();
    int kept = 0;
    for (auto& [path, v] : views_) {
        if (path == path_u8(kManual)) continue;  // the next build's manual may differ
        std::error_code ec;
        fs::path rel = fs::relative(u8path(path), store_.root(), ec);
        std::string key = path_u8(rel);
        if (ec || key.empty() || key.rfind("..", 0) == 0 || key.find('=') != std::string::npos) continue;
        if (++kept > 500) break;
        kv["view:" + key] = std::to_string(v.first.line) + " " + std::to_string(v.first.col) + " " + std::to_string((int)v.second);
    }
    if (!fullscreen_ && !glfwGetWindowAttrib(win_, GLFW_ICONIFIED) && !glfwGetWindowAttrib(win_, GLFW_MAXIMIZED)) {
        int x, y, w, h;
        glfwGetWindowPos(win_, &x, &y);
        glfwGetWindowSize(win_, &w, &h);
        kv["x"] = std::to_string(x);
        kv["y"] = std::to_string(y);
        kv["width"] = std::to_string(w);
        kv["height"] = std::to_string(h);
    }
    save_kv(state_path_, kv);
}
