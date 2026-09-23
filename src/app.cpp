#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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
    if (state["reader"] == "1") editor_.set_reader(true);

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
        atomic_write(cfg_path_, default_config_text(binding_docs()), nullptr);
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
    apply_bindings(issues);

    fs::path root = expand_user(cfg_.notes_dir);
    fs::create_directories(root, ec);
    if (ec) issues.push_back({cfg_.line_of("general.notes_dir"), "can't create this folder: " + ec.message()});
    store_.set_root(root);
    if (!Store::check_name(cfg_.default_space, "space").empty()) {
        issues.push_back({cfg_.line_of("general.default_space"), "not a valid folder name, using \"default\""});
        cfg_.default_space = "default";
    }
    if (!Store::check_name(cfg_.daily_space, "space").empty()) {
        issues.push_back({cfg_.line_of("general.daily_space"), "not a valid folder name, using \"daily\""});
        cfg_.daily_space = "daily";
    }

    // Problems show on their lines when the config is open in delta;
    // otherwise the first one goes to the status bar.
    config_issues_ = (int)issues.size();
    std::map<int, std::string> diag;
    for (auto& is : issues)
        if (is.line >= 0 && !diag.count(is.line)) diag[is.line] = is.msg;
    if (is_config()) editor_.set_diagnostics(diag);
    config_diag_ = std::move(diag);
    if (!issues.empty() && !is_config()) {
        const ConfigIssue& first = issues[0];
        std::string msg = "Config: " + (first.line >= 0 ? "line " + std::to_string(first.line + 1) + ", " : std::string()) + first.msg;
        if (issues.size() > 1) msg += " (+" + std::to_string(issues.size() - 1) + " more)";
        status(msg, true);
    }
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

void App::apply_bindings(std::vector<ConfigIssue>& issues) {
    for (auto& c : cmds_) {
        auto it = cfg_.keys.find(c.id);
        std::vector<std::string> bad;
        c.chords = parse_chords(it != cfg_.keys.end() ? it->second : c.defaults, &bad);
        for (auto& b : bad) issues.push_back({cfg_.line_of("keys." + c.id), "can't read the chord \"" + b + "\""});
    }
    for (auto& [id, v] : cfg_.keys)
        if (!find_command(id)) issues.push_back({cfg_.line_of("keys." + id), "no command with this name"});
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
    add("palette.line", "Go to line", "ctrl+g", [this] { open_palette(":"); }, false, true);
    add("palette.search", "Search notes in this space", "ctrl+f ctrl+shift+f", [this] {
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
        editor_.set_reader(!editor_.reader());
        fade_start_ = ImGui::GetTime();
    }, false, false);
    add("view.line_numbers", "Toggle line numbers", "", [this] { cfg_.line_numbers = !cfg_.line_numbers; });
    add("view.zoom_in", "Zoom in", "ctrl+= ctrl+numpad+", [this] { zoom_ = std::min(3.0f, zoom_ * 1.1f); status("Zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_out", "Zoom out", "ctrl+- ctrl+numpad-", [this] { zoom_ = std::max(0.5f, zoom_ / 1.1f); status("Zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%"); }, true, true);
    add("view.zoom_reset", "Reset zoom", "ctrl+0", [this] { zoom_ = 1; status("Zoom 100%"); }, false, true);
    add("view.fullscreen", "Toggle full screen", "f11", [this] { toggle_fullscreen(); }, false, true);

    // App
    add("config.open", "Edit config", "ctrl+,", [this] { open_config(); });
    add("config.open_external", "Open config in another app", "", [this] {
        if (!open_with_default_app(cfg_path_)) status("Couldn't open " + path_u8(cfg_path_), true);
    });
    add("config.reload", "Reload config", "", [this] { load_settings(); status("Config reloaded"); });
    add("app.open_folder", "Show this space's folder", "", [this] { open_with_default_app(store_.root() / u8path(space_)); });
    add("app.quit", "Quit", "ctrl+q", [this] { quit_ = true; }, false, true);

    // Editing
    add("edit.undo", "Undo", "ctrl+z", ed(&Editor::undo), true);
    add("edit.redo", "Redo", "ctrl+y ctrl+shift+z", ed(&Editor::redo), true);
    add("edit.cut", "Cut", "ctrl+x", ed(&Editor::cut));
    add("edit.copy", "Copy", "ctrl+c", [this] { if (note_) editor_.copy(); });
    add("edit.paste", "Paste", "ctrl+v", ed(&Editor::paste), true);
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
        editor_.set_reader(false);
    } else if (!config && was_config) {
        editor_.set_reader(reader_pref_);
    }
    editor_.set_syntax(config ? Syntax::Ini : Syntax::Markdown);
    editor_.set_text(text);
    note_ = n;
    if (config) editor_.set_diagnostics(config_diag_);
    else space_ = n.space;
    saved_version_ = seen_version_ = editor_.version();
    disk_mtime_ = mtime_of(n.path);
    if (auto it = views_.find(path_u8(n.path)); it != views_.end()) editor_.restore_view(it->second.first, it->second.second);
    fade_start_ = ImGui::GetTime();
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
    fade_start_ = ImGui::GetTime();
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
    palette_.prompt("rename: ", note_->name, "rename to", [this](const std::string& v) {
        save();
        std::string old = path_u8(note_->path);
        NoteRef n = *note_;
        if (auto e = store_.rename_note(n, v); !e.empty()) { status(e, true); return; }
        views_.erase(old);
        for (auto& h : history_)
            if (path_u8(h.path) == old) h = n;
        note_ = n;
        disk_mtime_ = mtime_of(n.path);
        set_title();
        status("Renamed to " + n.name);
    });
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

    if (c == '@') {
        if (!note_) return;
        auto heads = editor_.headings();
        total = (int)heads.size();
        for (auto& h : heads) {
            PaletteItem it;
            it.label = h.text.empty() ? "(untitled)" : h.text;
            if (!fuzzy_match(q, it.label, it.score, &it.hl)) continue;
            it.indent = q.empty() ? h.level - 1 : 0;
            it.meta = std::to_string(h.line + 1);
            int line = h.line;
            it.action = [this, line] { editor_.goto_line(line); };
            out.push_back(std::move(it));
        }
        sort();
        return;
    }

    if (c == ':') {
        if (!note_) return;
        total = -2;  // no count for a line number
        int lines = (int)editor_.lines().size();
        PaletteItem it;
        char* end = nullptr;
        long n = std::strtol(q.c_str(), &end, 10);
        if (!q.empty() && *end == 0 && n >= 1) {
            int line = (int)std::min<long>(n, lines);
            it.label = "go to line " + std::to_string(line);
            it.meta = "of " + std::to_string(lines);
            it.action = [this, line] { editor_.goto_line(line - 1); };
        } else {
            it.label = "a line number, 1 to " + std::to_string(lines);
        }
        out.push_back(std::move(it));
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
    bool pal = palette_.is_open();
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

void App::draw_empty(ImVec2 min, ImVec2 max, float page) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = base_size(), em = s / kEm;
    float a = anim::ease_out(anim::progress(fade_start_, ImGui::GetTime(), 0.14f)) * page;
    struct Row { std::string key, text; };
    std::vector<Row> rows;
    auto add = [&](const char* id, std::string text) {
        std::string k = chord_for(id);
        if (!k.empty()) rows.push_back({k, std::move(text)});
    };
    add("note.new", "New note in " + space_);
    add("note.daily", "Open today's daily note");
    add("palette.notes", "Find a note");
    add("palette.spaces", "Switch space");
    add("palette.commands", "All commands");

    float ks = std::round(s * 0.8f);
    float row_h = std::round(em * 1.9f);
    float key_w = 0;
    for (auto& r : rows) key_w = std::max(key_w, mono_->CalcTextSizeA(ks, FLT_MAX, 0, r.key.c_str()).x);
    float text_w = 0;
    for (auto& r : rows) text_w = std::max(text_w, sans_->CalcTextSizeA(s, FLT_MAX, 0, r.text.c_str()).x);
    float gap = em * 1.5f;
    float total_w = key_w + gap + text_w;
    float title_h = std::round(em * 2.6f);
    float total_h = title_h + rows.size() * row_h;
    float x = std::round((min.x + max.x - total_w) * 0.5f);
    float y = std::round((min.y + max.y - total_h) * 0.45f);

    std::string title = "No notes in " + space_ + " yet";
    if (store_.note_count(space_) > 0) title = "No note open";
    dl->AddText(sans_bold_, s, ImVec2(x, y), col_alpha(T.bright, a), title.c_str());
    y += title_h;
    for (auto& r : rows) {
        float kw = mono_->CalcTextSizeA(ks, FLT_MAX, 0, r.key.c_str()).x;
        dl->AddText(mono_, ks, ImVec2(std::round(x + key_w - kw), std::round(y + (s - ks) * 0.7f)), col_alpha(T.accent, a), r.key.c_str());
        dl->AddText(sans_, s, ImVec2(x + key_w + gap, y), col_alpha(T.dim, a), r.text.c_str());
        y += row_h;
    }
}

void App::draw_status(ImVec2 min, ImVec2 max) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float s = std::round(base_size() * 0.86f);
    float pad = std::round(16 * dpi_);
    float ty = std::round(min.y + (max.y - min.y - s) * 0.5f);
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
        put_right(editor_.reader() ? "read" : "edit", editor_.reader() ? T.accent : T.dim);
        if (!editor_.reader()) {
            TextPos c = editor_.cursor();
            const std::string& line = editor_.lines()[c.line];
            int col = 1;
            for (int i = 0; i < c.col && i < (int)line.size(); i++)
                if (!utf8::is_cont((unsigned char)line[i])) col++;
            put_right(std::to_string(c.line + 1) + ":" + std::to_string(col), T.dim);
        }
        if (is_config()) {
            int n = config_issues_;
            put_right(n == 0 ? "no problems" : std::to_string(n) + (n == 1 ? " problem" : " problems"), n ? T.text : T.dim);
        } else {
            int w = editor_.word_count();
            put_right(std::to_string(w) + (w == 1 ? " word" : " words"), T.dim);
        }
        if (editor_.has_selection()) put_right(std::to_string(editor_.selection_length()) + " selected", T.dim);
    }

    // Transient message, between the two.
    double now = ImGui::GetTime();
    float age = (float)(now - status_time_);
    if (!status_.empty() && age < 3.4f) {
        float a = age < 3.0f ? 1.0f : 1.0f - (age - 3.0f) / 0.4f;
        float in = anim::ease_out(std::min(1.0f, age / 0.1f));
        if (age < 0.1f) anim::active = true;
        if (age >= 3.0f) anim::active = true;
        ImU32 col = status_error_ ? T.accent : T.text;
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

    // Commands passed on the command line (used for testing and scripting).
    if (!pending_run_.empty()) {
        auto run = std::move(pending_run_);
        pending_run_.clear();
        for (auto& id : run) {
            if (id.rfind("open:", 0) == 0) { open_by_name(id.substr(5)); continue; }
            if (id.rfind("palette:", 0) == 0) { open_palette(id.substr(8)); continue; }
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
    float fade = anim::ease_out(anim::progress(fade_start_, now, 0.14f));
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
        p.blank_line = cfg_.blank_line_height;
        p.scroll_past_end = cfg_.scroll_past_end;
        p.keyboard = !palette_.is_open() && !fired;
        p.mouse = !palette_.visible();
        p.alpha = fade * page;
        if (fired) io.InputQueueCharacters.resize(0);
        editor_.draw(p);
    } else {
        draw_empty(min, max, page);
    }
    draw_status(ImVec2(min.x, max.y), ImVec2(max.x, max.y + bar_h));
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
    std::map<std::string, std::string> kv;
    kv["space"] = space_;
    kv["note"] = note_ ? note_->name : "";
    kv["zoom"] = std::to_string(zoom_);
    kv["reader"] = editor_.reader() ? "1" : "0";
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
