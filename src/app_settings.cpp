// Settings: the config loaded and checked, its problems, the fonts and theme
// it picks (and the pickers for them), and a setting written back. The file
// and its settings are config.h's; fonts are found on the font worker
// (fonts.h) and loaded here, between frames.
#include <algorithm>
#include <set>

#include "app.h"
#include "imgui_freetype.h"
#include "textfile.h"
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

void App::load_settings() {
    std::vector<ConfigIssue> issues;
    std::error_code ec;
    if (!fs::exists(cfg_path_, ec)) {
        fs::create_directories(cfg_path_.parent_path(), ec);
        atomic_write(cfg_path_, default_config_text(), nullptr);
    }
    Config c;
    if (!load_config(cfg_path_, c, issues)) issues.push_back({-1, "couldn't read " + path_u8(cfg_path_)});
    cfg_ = std::move(c);
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
        c.FontLoader = spaced_font_loader();  // letter_spacing
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
    // Letter spacing is in the glyphs, so a change makes them again.
    bool spaced = false;
    for (ImFont* f : {sans_, sans_bold_}) spaced |= set_letter_spacing(f, cfg_.letter_spacing);
    for (ImFont* f : {mono_, mono_bold_}) spaced |= set_letter_spacing(f, cfg_.mono_letter_spacing);
    if (spaced) editor_.fonts_changed();

    if (preview_want_ != preview_asked_) {
        preview_asked_ = preview_want_;
        if (!preview_want_.empty()) {
            bool mono = preview_want_[0] == 'm';
            font_loader_.preview(preview_want_.substr(2), find_setting("editor", mono ? "mono_font" : "font")->def);
        }
    }
    // Nothing wanted any more (the palette closed), or something else now.
    if (!preview_loaded_.empty() && preview_loaded_ != preview_want_) {
        if (preview_font_) {
            set_letter_spacing(preview_font_, 0);
            ImGui::GetIO().Fonts->RemoveFont(preview_font_);
        }
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
        if (pl.problem.empty()) {
            preview_font_ = load_font(pl.regular, pl.pick.regular.index, mono ? g_font_mono : g_font_sans, mono ? g_font_mono_size : g_font_sans_size);
            set_letter_spacing(preview_font_, mono ? cfg_.mono_letter_spacing : cfg_.letter_spacing);
        }
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
    // Their spacing is forgotten first (apply_fonts spaces the new ones).
    for (ImFont* f : {sans_, sans_bold_, mono_, mono_bold_}) set_letter_spacing(f, 0);
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
    // Underlines just `part` of the setting's value `v`.
    auto underline = [&](ConfigIssue& is, const std::string& key, const std::string& v, const std::string& part) {
        auto w = c.where.find(key);
        if (w == c.where.end() || is.line < 0) return;
        size_t at = v.find(part);
        if (at != std::string::npos) { is.col0 = w->second.val0 + (int)at; is.col1 = is.col0 + (int)part.size(); }
    };
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
            underline(is, "keys." + id, v, b);
            issues.push_back(is);
        }
    }
    // A key you gave a command that another has where both work: the inner
    // scope's runs, or in one scope the first listed (see dispatch_keys).
    auto chords_of = [&](const Command& cmd) {
        auto it = c.keys.find(cmd.id);
        return parse_chords(it != c.keys.end() ? it->second : cmd.defaults);
    };
    auto same = [](const Chord& a, const Chord& b) { return a.key == b.key && a.mods == b.mods; };
    auto overlap = [](Scope a, Scope b) {
        if (a > b) std::swap(a, b);
        return a == b || a == Scope::App || (a == Scope::Page && b == Scope::Tag) || (a == Scope::Scoped && b == Scope::Notes);
    };
    for (size_t i = 0; i < cmds_.size(); i++) {
        auto set = c.keys.find(cmds_[i].id);
        if (set == c.keys.end() || set->second == cmds_[i].defaults) continue;
        for (auto& ch : chords_of(cmds_[i]))
            for (size_t j = 0; j < cmds_.size(); j++) {
                if (j == i || !overlap(cmds_[i].scope, cmds_[j].scope)) continue;
                auto theirs = chords_of(cmds_[j]);
                if (std::none_of(theirs.begin(), theirs.end(), [&](const Chord& o) { return same(ch, o); })) continue;
                int di = depth(cmds_[i].scope), dj = depth(cmds_[j].scope);
                const std::string& wins = di != dj ? (di > dj ? cmds_[i].id : cmds_[j].id) : cmds_[std::min(i, j)].id;
                issues.push_back(issue_at(c, "keys." + cmds_[i].id, true,
                                          chord_str(ch) + " is also the key for " + cmds_[j].id + "; " + wins + " runs where both work"));
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
            underline(is, key, v, l);
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
    TextFile f;
    f.load(cfg_path_);  // none yet: written from scratch
    apply_edit(f.lines, config_set(f.lines, section, key, value));
    std::string err;
    if (!f.save(cfg_path_, &err)) {
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
