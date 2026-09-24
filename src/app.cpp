#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>

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

// ---------------------------------------------------------------------------
// Setup

bool App::init(GLFWwindow* window, const AppOptions& opts) {
    win_ = window;
    images_.start();
    cfg_path_ = opts.config_path.empty() ? config_dir() / "delta.ini" : opts.config_path;
    state_path_ = cfg_path_.parent_path() / "state.ini";
    pending_run_ = opts.run;

    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    sans_ = io.Fonts->AddFontFromMemoryTTF((void*)g_font_sans, (int)g_font_sans_size, 17.0f, &fc);
    mono_ = io.Fonts->AddFontFromMemoryTTF((void*)g_font_mono, (int)g_font_mono_size, 17.0f, &fc);
    sans_bold_ = io.Fonts->AddFontFromMemoryTTF((void*)g_font_sans_bold, (int)g_font_sans_bold_size, 17.0f, &fc);
    mono_bold_ = io.Fonts->AddFontFromMemoryTTF((void*)g_font_mono_bold, (int)g_font_mono_bold_size, 17.0f, &fc);
    // Glyphs Noto doesn't have (arrows, symbols) fall back to the system.
#ifdef _WIN32
    const char* fallbacks[] = {"C:\\Windows\\Fonts\\seguisym.ttf", "C:\\Windows\\Fonts\\seguiemj.ttf"};
    for (ImFont* target : {sans_, mono_, sans_bold_, mono_bold_}) {
        for (const char* path : fallbacks) {
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;
            ImFontConfig m;
            m.MergeMode = true;
            m.DstFont = target;
            io.Fonts->AddFontFromFileTTF(path, 17.0f, &m);
        }
    }
#endif

    register_commands();
    load_settings();

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

    build_theme(cfg_, issues);
    apply_imgui_style();
    style_native_window(win_, col_rgb(T.bg), col_rgb(T.dim), T.dark);
    anim::enabled = cfg_.animations;
    anim::speed = cfg_.animation_speed;
    apply_bindings();
    check_settings(cfg_, issues);

    fs::path root = expand_user(cfg_.notes_dir);
    fs::create_directories(root, ec);
    if (ec) issues.push_back(issue_at(cfg_, "general.notes_dir", true, "can't create this folder: " + ec.message()));
    store_.set_root(root);
    image_store_.set_root(root);
    if (!Store::check_name(cfg_.default_space, "space").empty()) cfg_.default_space = "default";
    if (!Store::check_name(cfg_.daily_space, "space").empty()) cfg_.daily_space = "daily";

    // Problems show on their lines when the config is open in delta;
    // otherwise the first one goes to the status bar.
    set_problems(std::move(issues));
    hints_version_ = UINT64_MAX;  // colour defaults follow the theme
    const auto& all = config_problems_;
    if (!all.empty() && !is_config()) {
        const ConfigIssue& first = all[0];
        std::string msg = "Config: " + (first.line >= 0 ? "line " + std::to_string(first.line + 1) + ", " : std::string()) + first.msg;
        if (all.size() > 1) msg += " (+" + std::to_string(all.size() - 1) + " more)";
        status(msg, true);
    }
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
}

// Keeps the problems in line order, for the count and F8.
void App::set_problems(std::vector<ConfigIssue> issues) {
    std::stable_sort(issues.begin(), issues.end(), [](const ConfigIssue& a, const ConfigIssue& b) {
        return a.line != b.line ? a.line < b.line : a.col0 < b.col0;
    });
    config_problems_ = std::move(issues);
}

// Hands the problems to the editor. Those on the line being typed on wait
// for a pause, so a half-typed value doesn't flash an error at you.
void App::show_problems(double now) {
    bool typing = now - last_change_ < 0.9;
    if (typing) next_problem_check_ = last_change_ + 0.9;
    std::vector<Diagnostic> d;
    std::set<int> seen;
    for (auto& is : config_problems_) {
        if (is.line < 0 || seen.count(is.line)) continue;  // one per line: the first
        if (typing && is.line == editor_.cursor().line) continue;
        seen.insert(is.line);
        d.push_back({is.line, is.col0, is.col1, is.msg});
    }
    editor_.set_diagnostics(d);
}

void App::goto_problem(int dir) {
    std::vector<const ConfigIssue*> list;
    for (auto& is : config_problems_)
        if (is.line >= 0) list.push_back(&is);
    if (list.empty()) { status("No problems in the config"); return; }
    const ConfigIssue* to = nullptr;
    if (!is_config()) {
        open_config();
        to = dir > 0 ? list.front() : list.back();
    } else {
        TextPos a = editor_.cursor(), b = editor_.anchor();
        TextPos from = dir > 0 ? std::max(a, b) : std::min(a, b);
        auto pos = [](const ConfigIssue* is) { return TextPos{is->line, std::max(0, is->col0)}; };
        if (dir > 0) {
            for (auto* is : list)
                if (from < pos(is)) { to = is; break; }
            if (!to) to = list.front();  // wrap
        } else {
            for (auto it = list.rbegin(); it != list.rend(); ++it)
                if (pos(*it) < from) { to = *it; break; }
            if (!to) to = list.back();
        }
    }
    if (!is_config()) return;
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
        const auto& lines = editor_.lines();
        bool header = false;
        int at = config_insert_line(lines, section, header);
        // Written at the end of the line before, so the new text ends
        // exactly after the value.
        std::string text = "\n";
        if (header) {
            bool empty_file = lines.size() == 1 && lines[0].empty();
            text = empty_file ? "" : lines.back().empty() ? "\n" : "\n\n";
            text += "[" + section + "]\n";
        }
        text += key + " = " + value;
        int prev = std::max(0, at - 1);
        TextPos end = editor_.insert_text({prev, (int)lines[prev].size()}, text);
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
    add("palette.notes", "Find a note in this space", "ctrl+p", [this] { open_palette(""); }, false, true);
    add("palette.commands", "Show all commands", "ctrl+shift+p f1", [this] { open_palette("/"); }, false, true);
    add("palette.all", "Find a note in any space", "ctrl+alt+p", [this] { open_palette("*"); }, false, true);
    add("palette.spaces", "Switch space", "ctrl+shift+s", [this] { open_palette("#"); }, false, true);
    add("palette.headings", "Jump to a heading", "ctrl+shift+o", [this] { open_palette("@"); }, false, true);
    add("palette.line", "Go to line", "ctrl+g", [this] { open_line(); }, false, true);
    add("find.open", "Find in this note", "ctrl+f", [this] { open_find(false); }, false, true);
    add("find.replace", "Find and replace in this note", "ctrl+h", [this] { open_find(true); }, false, true);
    add("nav.heading_next", "Go to the next heading", "ctrl+alt+down", [this] { if (editing()) editor_.goto_heading(1); }, true);
    add("nav.heading_prev", "Go to the previous heading", "ctrl+alt+up", [this] { if (editing()) editor_.goto_heading(-1); }, true);
    add("edit.expand_selection", "Expand selection", "alt+shift+right", [this] { if (editing()) editor_.expand_selection(); }, true);
    add("edit.shrink_selection", "Shrink selection", "alt+shift+left", [this] { if (editing()) editor_.shrink_selection(); }, true);
    add("edit.follow_link", "Follow the link at the caret", "alt+enter", [this] {
        LinkSpan link;
        if (!editing()) return;
        if (editor_.link_at_caret(link)) open_link(link);
        else status("No link at the caret");
    });
    add("palette.search", "Search notes in this space", "ctrl+shift+f", [this] {
        std::string sel = note_ && editor_.has_selection() && editor_.cursor().line == editor_.anchor().line ? editor_.selected_text() : "";
        open_palette("?" + sel);
    }, false, true);
    add("nav.back", "Go back to the previous note", "alt+left", [this] { history_step(-1); });
    add("nav.forward", "Go forward to the next note", "alt+right", [this] { history_step(1); });

    // Notes and spaces
    add("note.new", "New note", "ctrl+n", [this] {
        palette_.prompt("new note: " + space_ + "/", "", "+ new note", [this](const std::string& v) { create_note(space_, v); });
    });
    add("note.daily", "Open today's daily note", "alt+d", [this] { open_daily(); });
    add("note.rename", "Rename note", "f2", [this] { rename_note(); });
    add("note.move", "Move note to another space", "", [this] { move_note(); });
    add("note.delete", "Delete note", "", [this] { delete_note(); });
    add("note.save", "Save note now", "ctrl+s", [this] { if (save()) status("Saved"); });
    add("space.new", "New space", "", [this] { new_space(); });
    add("space.rename", "Rename this space", "", [this] { rename_space(); });
    add("space.delete", "Delete this space", "", [this] { delete_space(); });

    // View
    add("view.reader", "Toggle reader mode", "ctrl+e", [this] {
        if (!note_ || is_config()) return;
        editor_.set_reader(!editor_.reader());  // morphs between the views
    }, false, false);
    add("view.line_numbers", "Toggle line numbers", "", [this] { cfg_.line_numbers = !cfg_.line_numbers; });
    add("view.zoom_in", "Zoom in", "ctrl+= ctrl+numpad+", [this] { zoom_ = std::min(3.0f, zoom_ * 1.1f); status("Zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_out", "Zoom out", "ctrl+- ctrl+numpad-", [this] { zoom_ = std::max(0.5f, zoom_ / 1.1f); status("Zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_reset", "Reset zoom", "ctrl+0", [this] { zoom_ = 1; status("Zoom 100%"); }, false, true);
    add("view.fullscreen", "Toggle full screen", "f11", [this] { toggle_fullscreen(); }, false, true);

    // App
    add("config.open", "Edit config", "ctrl+,", [this] { open_config(); });
    add("config.add_setting", "Add a setting", "", [this] { add_setting(); });
    add("config.open_external", "Open config in another app", "", [this] {
        if (!open_with_default_app(cfg_path_)) status("Couldn't open " + path_u8(cfg_path_), true);
    });
    add("config.next_problem", "Go to the next config problem", "f8", [this] { goto_problem(1); }, true);
    add("config.prev_problem", "Go to the previous config problem", "shift+f8", [this] { goto_problem(-1); }, true);
    add("config.reload", "Reload config", "", [this] { load_settings(); status("Config reloaded"); });
    add("app.open_folder", "Show this space's folder", "", [this] { open_with_default_app(store_.root() / u8path(space_)); });
    add("app.quit", "Quit", "ctrl+q", [this] { quit_ = true; }, false, true);

    // Editing
    add("edit.undo", "Undo", "ctrl+z", ed(&Editor::undo), true);
    add("edit.redo", "Redo", "ctrl+y ctrl+shift+z", ed(&Editor::redo), true);
    add("edit.cut", "Cut", "ctrl+x", [this] { if (editing()) copy_out(true); });
    add("edit.copy", "Copy", "ctrl+c", [this] { if (note_) copy_out(false); });
    add("edit.paste", "Paste", "ctrl+v", [this] { paste(false); }, true);
    add("edit.paste_plain", "Paste as plain text", "ctrl+shift+v", [this] { paste(true); }, true);
    add("image.view", "View image", "", [this] {
        int tile = 0;
        int line = editor_.image_for_caret(&tile);
        if (line >= 0) open_viewer(line, tile);
        else status("Pick an image to view it");
    });
    add("edit.select_all", "Select all", "ctrl+a", ed(&Editor::select_all));
    add("edit.select_line", "Select line", "ctrl+l", ed(&Editor::select_line), true);
    add("edit.line_up", "Move line up", "alt+up", [this] { if (editing()) editor_.move_lines(-1); }, true);
    add("edit.line_down", "Move line down", "alt+down", [this] { if (editing()) editor_.move_lines(1); }, true);
    add("edit.duplicate_up", "Duplicate line up", "shift+alt+up", [this] { if (editing()) editor_.duplicate_lines(-1); }, true);
    add("edit.duplicate_down", "Duplicate line down", "shift+alt+down", [this] { if (editing()) editor_.duplicate_lines(1); }, true);
    add("edit.delete_line", "Delete line", "ctrl+shift+k", ed(&Editor::delete_lines), true);
    add("edit.toggle_task", "Toggle task checkbox", "ctrl+enter", ed(&Editor::toggle_task));
    add("edit.bold", "Bold", "ctrl+b", [this] { if (editing()) editor_.wrap_selection("**"); });
    add("edit.italic", "Italic", "ctrl+i", [this] { if (editing()) editor_.wrap_selection("*"); });
    add("edit.code", "Inline code", "ctrl+`", [this] { if (editing()) editor_.wrap_selection("`"); });
    add("edit.strike", "Strikethrough", "ctrl+shift+x", [this] { if (editing()) editor_.wrap_selection("~~"); });
    add("edit.indent", "Indent", "ctrl+]", ed(&Editor::indent), true);
    add("edit.outdent", "Outdent", "ctrl+[", ed(&Editor::outdent), true);
}

// ---------------------------------------------------------------------------
// Notes

void App::set_title() {
    std::string t = note_ ? note_->name + " - delta" : "delta";
    glfwSetWindowTitle(win_, t.c_str());
}

void App::remember_view() {
    if (note_) views_[path_u8(note_->path)] = {editor_.cursor(), editor_.scroll()};
}

bool App::save() {
    if (!note_ || editor_.version() == saved_version_) return true;
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
    std::string err;
    std::error_code ec;
    fs::create_directories(note_->path.parent_path(), ec);
    if (!atomic_write(note_->path, text, &err)) {
        status("Couldn't save: " + err, true);
        return false;
    }
    saved_version_ = editor_.version();
    disk_mtime_ = mtime_of(note_->path);
    if (is_config()) load_settings();  // edits apply as soon as they're saved
    return true;
}

void App::open_note(const NoteRef& n, bool push_history) {
    close_bar();
    save();
    remember_view();
    std::string text;
    if (!read_file(n.path, text)) {
        status("Couldn't open " + n.name, true);
        return;
    }
    crlf_ = text.find("\r\n") != std::string::npos;

    // The config is edited like a note, but as ini text and never in reader
    // mode; the reader preference comes back when you return to notes.
    bool was_config = is_config();
    bool config = n.path == cfg_path_;
    if (config && !was_config) {
        reader_pref_ = editor_.reader();
        editor_.set_reader(false, false);
    } else if (!config && was_config) {
        editor_.set_reader(reader_pref_, false);
    }
    editor_.set_syntax(config ? Syntax::Ini : Syntax::Markdown);
    images_.set_note_dir(n.path.parent_path());  // images are found from the note's folder
    editor_.set_text(text);
    note_ = n;
    if (config) show_problems(ImGui::GetTime());
    else space_ = n.space;
    hints_version_ = UINT64_MAX;
    saved_version_ = seen_version_ = editor_.version();
    disk_mtime_ = mtime_of(n.path);
    if (auto it = views_.find(path_u8(n.path)); it != views_.end()) editor_.restore_view(it->second.first, it->second.second);
    fade_start_ = anim::clock;
    if (push_history) {
        if (hist_pos_ >= 0 && hist_pos_ < (int)history_.size() && history_[hist_pos_].path == n.path) {
            // same note, nothing to record
        } else {
            history_.resize(hist_pos_ + 1);
            history_.push_back(n);
            hist_pos_ = (int)history_.size() - 1;
        }
    }
    set_title();
}

void App::close_note() {
    save();
    remember_view();
    note_.reset();
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
    status("Created " + name);
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
        if (!open_url(t)) status("Only http, https and mailto links open from notes.", true);
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
    target = trim(target);
    if (auto h = target.find('#'); h != std::string::npos) target = trim(target.substr(0, h));
    if (target.empty()) return;
    std::string space, name = target;
    if (auto slash = target.find('/'); slash != std::string::npos) {
        space = trim(target.substr(0, slash));
        name = trim(target.substr(slash + 1));
    }
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
    if (is_config()) { status("The config file stays where it is. Open a note to do that."); return; }
    open_bar(Bar::Rename);  // see draw_rename_bar
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", note_->name.c_str());
    bar_last_ = bar_buf_;
}

void App::move_note() {
    if (!note_) return;
    if (is_config()) { status("The config file stays where it is. Open a note to do that."); return; }
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
            note_ = n;
            space_ = sp;
            disk_mtime_ = mtime_of(n.path);
            status("Moved to " + sp);
        };
        items.push_back(it);
    }
    if (items.empty()) { status("There's no other space yet. Make one with New space."); return; }
    palette_.pick("move to: ", items);
}

void App::delete_note() {
    if (!note_) return;
    if (is_config()) { status("The config file stays where it is. Open a note to do that."); return; }
    NoteRef n = *note_;
    PaletteItem yes, no;
    yes.label = "delete \"" + n.name + "\"";
    yes.meta = "to .trash";
    yes.emphasis = true;
    yes.action = [this, n] {
        save();
        if (auto e = store_.trash_note(n); !e.empty()) { status(e, true); return; }
        views_.erase(path_u8(n.path));
        history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const NoteRef& h) { return h.path == n.path; }), history_.end());
        hist_pos_ = std::min(hist_pos_, (int)history_.size() - 1);
        note_.reset();
        auto rest = store_.notes(n.space);
        if (!rest.empty()) open_note(rest.front(), false);
        else close_note();
        status("Deleted " + n.name + " (it's in .trash)");
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
        status("Switched to " + name);
    });
}

void App::rename_space() {
    palette_.prompt("rename space: ", space_, "rename to", [this](const std::string& v) {
        save();
        std::string from = space_, to = trim(v);
        if (auto e = store_.rename_space(from, to); !e.empty()) { status(e, true); return; }
        space_ = to;
        views_.clear();
        for (auto& h : history_)
            if (h.space == from) h = store_.ref(to, h.name);
        if (note_) {
            note_ = store_.ref(to, note_->name);
            disk_mtime_ = mtime_of(note_->path);
        }
        status("Renamed space to " + to);
    });
}

void App::delete_space() {
    std::string sp = space_;
    if (store_.note_count(sp) > 0) { status(quote(sp) + " still has notes. Move or delete them first.", true); return; }
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
        status("Deleted space " + sp);
    };
    no.label = "keep it";
    no.action = [] {};
    palette_.pick("delete space? ", {yes, no});
}

void App::history_step(int dir) {
    int p = hist_pos_ + dir;
    while (p >= 0 && p < (int)history_.size()) {
        std::error_code ec;
        if (fs::exists(history_[p].path, ec)) {
            hist_pos_ = p;
            open_note(history_[p], false);
            return;
        }
        history_.erase(history_.begin() + p);
        if (dir < 0) p--;
    }
}

// ---------------------------------------------------------------------------
// Palette

void App::open_palette(const std::string& prefix) {
    close_bar();
    cache_notes_ = store_.notes(space_);
    cache_all_.clear();
    cache_spaces_ = store_.spaces();
    cache_text_.clear();
    // Pressing the same shortcut again closes it.
    if (palette_.is_open() && !palette_.text().empty() == !prefix.empty() &&
        (prefix.empty() || palette_.text()[0] == prefix[0])) {
        palette_.close();
        return;
    }
    palette_.open(prefix);
}

void App::build_palette(const std::string& text, std::vector<PaletteItem>& out, int& total) {
    char c = text.empty() ? 0 : text[0];
    bool prefixed = c && std::strchr("#*@/:?", c);
    std::string q = trim(prefixed ? text.substr(1) : text);
    auto sort = [&] {
        if (!q.empty()) std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score > b.score; });
    };

    if (!prefixed || c == '*') {
        bool all = c == '*';
        if (all && cache_all_.empty()) cache_all_ = store_.all_notes();
        const auto& notes = all ? cache_all_ : cache_notes_;
        total = (int)notes.size();
        bool exact = false;
        for (auto& n : notes) {
            PaletteItem it;
            it.label = all ? n.space + "/" + n.name : n.name;
            if (!fuzzy_match(q, it.label, it.score, &it.hl)) continue;
            if (iequals(n.name, q)) exact = true;
            bool current = note_ && note_->path == n.path;
            it.meta = current ? "open" : ago(n.mtime);
            it.action = [this, n] { open_note(n); };
            out.push_back(std::move(it));
            if (out.size() > 300) break;
        }
        sort();
        if (!q.empty() && !exact) {
            std::string sp = space_, name = q;
            if (all && q.find('/') != std::string::npos) {
                sp = trim(q.substr(0, q.find('/')));
                name = trim(q.substr(q.find('/') + 1));
            }
            PaletteItem it;
            it.label = "+ new note \"" + name + "\"";
            it.meta = sp;
            it.emphasis = true;
            it.action = [this, sp, name] { create_note(sp, name); };
            out.push_back(std::move(it));
        }
        return;
    }

    if (c == '#') {
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
        if (!note_) return;
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
            it.action = [this, line] { editor_.goto_line(line); };
            out.push_back(std::move(it));
        }
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
                // Show a window around the match.
                size_t from = at > 40 ? at - 40 : 0;
                while (from > 0 && utf8::is_cont((unsigned char)lines[i][from])) from--;
                std::string snippet = (from ? "..." : "") + lines[i].substr(from, 160);
                int hl0 = (int)(at - from + (from ? 3 : 0));
                PaletteItem it;
                it.label = trim(snippet);
                int lead = (int)(snippet.size() - snippet.substr(snippet.find_first_not_of(" \t") == std::string::npos ? 0 : snippet.find_first_not_of(" \t")).size());
                for (int k = 0; k < (int)q.size(); k++) it.hl.push_back(hl0 - lead + k);
                it.meta = n.name + ":" + std::to_string(i + 1);
                TextPos a{i, (int)at}, b{i, (int)(at + q.size())};
                it.action = [this, n, a, b] {
                    if (!note_ || note_->path != n.path) open_note(n);
                    editor_.select_range(a, b);
                };
                out.push_back(std::move(it));
            }
        };
        if (note_) scan(*note_, editor_.lines());
        for (auto& n : cache_notes_) {
            if (note_ && n.path == note_->path) continue;
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
            scan(n, it->second);
        }
        return;
    }

    if (c == '/') {
        total = (int)std::count_if(cmds_.begin(), cmds_.end(), [](const Command& c) { return c.in_palette; });
        for (auto& cmd : cmds_) {
            if (!cmd.in_palette) continue;
            PaletteItem it;
            it.label = cmd.title;
            // Terminal vocabulary: lowercase, like the rest of the palette.
            if (!it.label.empty()) it.label[0] = (char)std::tolower((unsigned char)it.label[0]);
            if (!fuzzy_match(q, it.label, it.score, &it.hl)) {
                std::vector<int> dummy;
                if (!fuzzy_match(q, cmd.id, it.score, &dummy)) continue;
                it.score -= 500;
            }
            if (!cmd.chords.empty()) it.meta = chord_str(cmd.chords.front());
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
    status_error_ = error;
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
    if (note_ && editor_.version() != saved_version_) t = std::min(t, last_change_ + cfg_.autosave_delay);
    double status_end = status_time_ + 3.4;
    if (status_end > now) t = std::min(t, status_end);
    if (is_config() && next_problem_check_ > now) t = std::min(t, next_problem_check_);
    return t;
}

void App::poll_files(double now) {
    if (now < next_poll_) return;
    next_poll_ = now + 1.0;
    auto m = mtime_of(cfg_path_);
    if (m != cfg_mtime_ && m != fs::file_time_type{}) {
        load_settings();
        if (status_.empty() || !status_error_) status("Config reloaded");
    }
    if (!note_) return;
    auto nm = mtime_of(note_->path);
    if (nm == fs::file_time_type{} || nm == disk_mtime_) return;
    disk_mtime_ = nm;
    if (editor_.version() != saved_version_) {
        status("This note changed on disk. Your next save will overwrite it.", true);
        return;
    }
    std::string text;
    if (read_file(note_->path, text)) {
        crlf_ = text.find("\r\n") != std::string::npos;
        editor_.reload_text(text);
        saved_version_ = seen_version_ = editor_.version();
    }
}

void App::toggle_fullscreen() {
    if (!fullscreen_) {
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
        const GLFWvidmode* vm = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(win_, mon, 0, 0, vm->width, vm->height, vm->refreshRate);
        fullscreen_ = true;
    } else {
        glfwSetWindowMonitor(win_, nullptr, win_x_, win_y_, win_w_, win_h_, 0);
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

    // Set on the page's lines, spaced like the editor's.
    float row_h = std::round(s * cfg_.line_height);
    float top = min.y + top_pad;
    int lines = std::max(1, (int)((max.y - top) / row_h));

    float ms = std::round(s * 0.9f);
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
        status(n ? "Replaced " + std::to_string(n) : "Nothing to replace");
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
    if (bar_ != Bar::Line) {
        bar_cur_ = editor_.cursor();
        bar_anchor_ = editor_.anchor();
        bar_scroll_ = editor_.scroll();
    }
    open_bar(Bar::Line);
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
                for (auto& h : history_)
                    if (path_u8(h.path) == old) h = n;
                note_ = n;
                disk_mtime_ = mtime_of(n.path);
                set_title();
                close_bar();
                status("Renamed to " + n.name);
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
    float s = std::round(base_size() * 0.86f);
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
        if (is_config()) {
            // The count is a mark like the ones on the lines; click it (or
            // press F8) to go to the next problem.
            int n = (int)config_problems_.size();
            std::string t = n == 0 ? "no problems" : std::to_string(n) + (n == 1 ? " problem" : " problems");
            float right = rx;
            put_right(t, n ? T.error : T.dim);
            ImVec2 mouse = ImGui::GetIO().MousePos;
            bool over = n && mouse.x >= right - W(t) && mouse.x < right && mouse.y >= min.y && mouse.y < max.y;
            if (over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !palette_.visible() && !viewer_.open) goto_problem(1);
        } else {
            int w = editor_.word_count();
            put_right(std::to_string(w) + (w == 1 ? " word" : " words"), T.dim);
        }
        // (Not for a picked image: its markdown's length means nothing.)
        if (editor_.has_selection() && editor_.image_for_caret() < 0) put_right(std::to_string(editor_.selection_length()) + " selected", T.dim);
    }

    // Transient message, between the two.
    double now = ImGui::GetTime();
    float age = (float)(now - status_time_);
    if (!status_.empty() && age < 3.4f) {
        float a = age < 3.0f ? 1.0f : 1.0f - (age - 3.0f) / 0.4f;
        float in = anim::ease_out(std::min(1.0f, age / 0.1f));
        if (age < 0.1f) anim::active = true;
        if (age >= 3.0f) anim::active = true;
        ImU32 col = status_error_ ? T.bright : T.text;
        float mx = std::max(left_end, std::round((min.x + max.x - W(status_)) * 0.5f));
        dl->PushClipRect(ImVec2(left_end, min.y), ImVec2(rx, max.y), true);
        dl->AddText(mono_, s, ImVec2(mx, ty), col_alpha(col, a * in), status_.c_str());
        dl->PopClipRect();
    } else if (age >= 3.4f) {
        status_.clear();
    }
}

void App::frame() {
    ImGuiIO& io = ImGui::GetIO();
    double now = ImGui::GetTime();
    images_.frame(now);
    poll_downloads();
    handle_drops();

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
            // "up" press and let go of the left button.
            if (id.rfind("mouse:", 0) == 0 || id == "down" || id == "up") {
                ImGuiIO& mio = ImGui::GetIO();
                float mx = 0, my = 0;
                if (id[0] == 'm' && std::sscanf(id.c_str() + 6, "%f:%f", &mx, &my) == 2) mio.AddMousePosEvent(mx, my);
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

    float s = base_size();
    float bar_h = std::round(s * 1.5f);
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
        p.sans_bold = sans_bold_;
        p.mono_bold = mono_bold_;
        p.size = s;
        p.line_height = cfg_.line_height;
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
        if (is_config()) {
            if (editor_.version() != hints_version_) {
                hints_version_ = editor_.version();
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
            }
            show_problems(now);
        }
        editor_.draw(p);
        // Double-click an image to view it (in the reader, a click will do).
        int img_line = -1, img_tile = 0;
        if (p.mouse && !editor_.over_image_handle() && editor_.image_at(io.MousePos, img_line, img_tile) &&
            (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || (editor_.reader() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
            open_viewer(img_line, img_tile);
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
    Palette::Frame pf{mono_, mono_bold_, s, dpi_, col_x, column, min.y + top_pad, max.y - top_pad, now, io.DeltaTime,
                      cfg_.palette_selection == "invert" ? Palette::Selection::Invert : Palette::Selection::Marker,
                      palette_caret, cfg_.cursor_blink && focused_};
    // The prompt names where you are, like a minibuffer.
    auto prompt_for = [this](char mode) -> std::string {
        switch (mode) {
            case '*': return "*/";
            case '#': return "spaces: ";
            case '@': return "headings: ";
            case ':': return "line: ";
            case '?': return "search " + space_ + ": ";
            case '/': return "commands: ";
            default: return space_ + "/";
        }
    };
    auto chosen = palette_.draw(
        pf, [this](const std::string& t, std::vector<PaletteItem>& out, int& total) { build_palette(t, out, total); }, prompt_for);
    if (chosen) chosen();
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
        if (editor_.version() != saved_version_ && now - last_change_ >= cfg_.autosave_delay) save();
    }
}

void App::shutdown() {
    save();
    images_.shutdown();  // while the GL context is still around
    std::map<std::string, std::string> kv;
    kv["space"] = space_;
    kv["note"] = note_ ? note_->name : "";
    kv["zoom"] = std::to_string(zoom_);
    kv["reader"] = editor_.reader() ? "1" : "0";
    // Where each note's caret and scroll were, so notes reopen where you left
    // them. Paths are relative to the notes folder.
    remember_view();
    int kept = 0;
    for (auto& [path, v] : views_) {
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
