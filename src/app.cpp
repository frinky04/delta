// The app's set-up and command table with where keys work; notes and spaces
// (opening, saving, reloading, renaming, moving, deleting), history and the
// note switcher; the palette's rows; and the frame: keys, file polling, the
// empty page and the status line. Features are in app_*.cpp.
#include "app.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <climits>
#include <cstring>
#include <set>

#include "commands.h"
#include "problems.h"
#include "spell_fix.h"
#include "theme.h"
#include "util.h"

extern const unsigned char g_manual[];
extern const std::size_t g_manual_size;

namespace {

std::tm local_tm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string ago(std::time_t t) {
    if (!t) return {};
    long long d = (long long)(std::time(nullptr) - t);
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + "m";
    if (d < 86400) return std::to_string(d / 3600) + "h";
    if (d < 86400 * 14) return std::to_string(d / 86400) + "d";
    if (d < 86400 * 90) return std::to_string(d / (86400 * 7)) + "w";
    char buf[32];
    std::tm tm = local_tm(t);
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

}  // namespace

Place App::here() const {
    Place p;
    if (note_) p = {*note_, editor_.cursor(), editor_.scroll()};
    return p;
}

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
    // Completing as you write: [[links]] to notes, #tags from every note.
    Completer link;
    link.trigger = md_link_word_start;
    link.items = [this](const std::string& w) { return link_completions(w); };
    link.min_chars = 0;
    link.close = "]]";
    editor_.add_completer(std::move(link));
    Completer tag;
    tag.trigger = tag_word_start;
    tag.items = [this](const std::string& w) { return tag_completions(w); };
    tag.lead = 1;
    editor_.add_completer(std::move(tag));

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
    // returns to it), with the ways out in the status line. A path to open
    // (delta.exe report.md) is opened instead, as if it were dropped.
    if (!opts.open.empty()) {
        open_paths({opts.open}, ImVec2(-1, -1));
    } else if (first_launch) {
        open_manual();
        std::string tip;
        for (auto [id, what] : {std::pair{"note.new", "new note"}, {"palette.notes", "find a note"}})
            if (std::string k = chord_for(id); !k.empty()) tip += (tip.empty() ? "" : " \xC2\xB7 ") + k + " " + what;
        if (!tip.empty()) status(tip);
    }
    set_title();
    return true;
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

std::string App::key_hint(const std::string& id, const std::string& what) const {
    std::string k = chord_for(id);
    return k.empty() ? "" : k + " to " + what;
}

bool App::in_scope(Scope s) const {
    bool page = !palette_.is_open() && bar_ == Bar::None;
    switch (s) {
        case Scope::App: return true;
        case Scope::Page: return page;
        case Scope::Tag: return page && tag_at_caret();
        case Scope::Slip: return page && slip_at_caret();
        case Scope::Visit: return page && is_visiting() && !is_manual();
        case Scope::Scoped: return palette_.is_open() && palette_.scoped();
        case Scope::Notes: return palette_.in_mode('.');
    }
    return false;
}

// Unreadable chords are skipped (check_settings reports them).
void App::apply_bindings() {
    for (auto& c : cmds_) {
        auto it = cfg_.keys.find(c.id);
        c.chords = parse_chords(it != cfg_.keys.end() ? it->second : c.defaults);
    }
}

void App::register_commands() {
    auto add = [&](const char* id, const char* title, const char* keys, std::function<void()> fn, bool repeat = false, Scope scope = Scope::Page,
                   char prefix = 0) {
        Command c;
        c.id = id;
        c.title = title;
        c.defaults = keys;
        c.run = std::move(fn);
        c.repeat = repeat;
        c.scope = scope;
        c.prefix = prefix;
        cmds_.push_back(std::move(c));
    };
    auto ed = [this](void (Editor::*fn)()) { return [this, fn] { if (editing()) (editor_.*fn)(); }; };

    // Navigation
    add("palette.notes", "find note in this space", "ctrl+o", [this] { open_palette("."); }, false, Scope::App, '.');
    add("palette.all", "find note in any space", "ctrl+alt+o", [this] { open_palette(".", true); }, false, Scope::App, '.');
    // A palette mode's own commands. Their keys work in the mode; picked
    // from the list of commands, they open it first.
    add("palette.sort", "sort notes by name or recent", "ctrl+r", [this] {
        sort_recent_ = !sort_recent_;
        if (palette_.in_mode('.')) palette_.rebuild();
        else open_palette(".");
    }, false, Scope::Notes, '.');
    add("palette.widen", "widen to every space, or back", "tab", [this] {
        if (palette_.is_open() && palette_.scoped()) palette_.set_wide(!palette_.wide());
        else open_palette(".", true);
    }, false, Scope::Scoped, '.');
    add("palette.commands", "show all commands", "ctrl+p", [this] { open_palette(""); }, false, Scope::App);
    add("palette.spaces", "switch space", "ctrl+shift+s", [this] { open_palette("/"); }, false, Scope::App, '/');
    add("palette.headings", "jump to heading", "ctrl+shift+o", [this] { open_palette("@"); }, false, Scope::App, '@');
    add("palette.line", "go to line", "ctrl+g", [this] { open_line(); }, false, Scope::App, ':');
    add("find.open", "find in this note", "ctrl+f", [this] { open_find(false); }, false, Scope::App);
    add("find.replace", "find and replace in this note", "ctrl+h", [this] { open_find(true); }, false, Scope::App);
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
    }, false, Scope::App, '?');
    add("palette.backlinks", "show notes that link here", "ctrl+shift+b", [this] { open_palette("["); }, false, Scope::App, '[');
    add("palette.tags", "find notes by tag", "ctrl+t", [this] { open_palette("#"); }, false, Scope::App, '#');
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
    add("tag.rename", "rename tag", "f2", [this] { pick_tag_rename(); }, false, Scope::Tag);
    add("note.move", "move note to another space", "", [this] { move_note(); });
    add("note.delete", "delete note", "", [this] { delete_note(); });
    add("note.restore", "restore deleted note", "", [this] { restore_note(); });
    add("note.save", "save note now", "ctrl+s", [this] {
        if (cfg_.format_on_save && is_note()) format_note(true);
        if (save(true)) status("saved");
    });
    add("note.reload", "reload note", "", [this] { reload_note(); });
    add("note.keep", "keep note", "ctrl+s", [this] { keep_note(); }, false, Scope::Visit);
    add("space.new", "new space", "", [this] { new_space(); });
    add("space.rename", "rename this space", "", [this] { rename_space(); });
    add("space.delete", "delete this space", "", [this] { delete_space(); });

    // View
    add("view.reader", "toggle reader mode", "ctrl+e", [this] {
        if (!note_ || is_config()) return;
        if (is_manual()) { status("the manual is read only"); return; }
        if (is_visiting()) { status("visiting docs are read only; " + key_hint("note.keep", "keep it as a note")); return; }
        editor_.set_reader(!editor_.reader());  // morphs between the views
    });
    add("view.line_numbers", "toggle line numbers", "", [this] { cfg_.line_numbers = !cfg_.line_numbers; });
    auto zoom = [this](float z) {
        zoom_ = std::clamp(z, 0.5f, 3.0f);
        status("zoom " + std::to_string((int)std::lround(zoom_ * 100)) + "%");
    };
    add("view.zoom_in", "zoom in", "ctrl+= ctrl+numpad+", [this, zoom] { zoom(zoom_ * 1.1f); }, true, Scope::App);
    add("view.zoom_out", "zoom out", "ctrl+- ctrl+numpad-", [this, zoom] { zoom(zoom_ / 1.1f); }, true, Scope::App);
    add("view.zoom_reset", "reset zoom", "ctrl+0", [zoom] { zoom(1); }, false, Scope::App);
    add("view.fullscreen", "toggle full screen", "f11", [this] { toggle_fullscreen(); }, false, Scope::App);

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
    add("edit.fix_slip", "fix style slip", "alt+.", [this] { fix_slip(); }, false, Scope::Slip);
    add("edit.format_note", "format note", "shift+alt+f", [this] { format_note(false); });
    add("edit.fix_spelling", "fix spelling, word by word", "ctrl+.", [this] { fix_spelling(); });
    add("view.spelling", "toggle spellcheck", "", [this] {
        cfg_.spell = !cfg_.spell;
        apply_spelling();
        status(cfg_.spell ? "spellcheck on" : "spellcheck off");
    });
    add("config.reload", "reload config", "", [this] { load_settings(); status("config reloaded"); });
    add("app.open_folder", "show this space's folder", "", [this] { open_with_default_app(store_.root() / u8path(space_)); });
    add("app.manual", "open manual", "f1", [this] { open_manual(); });
    add("app.quit", "quit", "ctrl+q", [this] { quit_ = true; }, false, Scope::App);

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

std::string App::text_to_save() const {
    std::string text = editor_.text();
    // A download still on its way is saved as the address it came from; the
    // placeholder is only ever on screen.
    for (auto& d : downloads_) {
        if (d->note != note_->path) continue;
        for (size_t at = text.find(d->marker); at != std::string::npos; at = text.find(d->marker, at))
            text.replace(at, d->marker.size(), d->url);
    }
    return text;
}

bool App::save(bool over_disk) {
    if (!note_ || is_visiting()) return true;
    std::string err;
    switch (file_.save(text_to_save(), editor_.version(), over_disk, &err)) {
    case NoteFile::Save::Clean: return true;
    case NoteFile::Save::Held: return false;
    case NoteFile::Save::Failed:
        status("couldn't save: " + err, true);
        return false;
    case NoteFile::Save::Saved: break;
    }
    if (is_config()) load_settings();  // edits apply as soon as they're saved
    return true;
}

// The note as it is on disk, in place of what's in the editor.
void App::reload_note() {
    if (!note_ || is_visiting()) return;
    std::string text;
    if (!file_.reload(text, nullptr)) {
        status("couldn't read " + note_->name + "; it may have been moved or deleted", true);
        return;
    }
    editor_.reload_text(text);
    file_.synced(editor_.version());
    seen_version_ = editor_.version();
}

// Leaving a note in conflict with disk: the file keeps the other version,
// and your edits are saved beside it as "name (yours)", so neither is lost.
void App::keep_yours() {
    if (!note_) {
        file_.close();  // no note (one just deleted, say): nothing of its file to keep
        return;
    }
    // Without edits in conflict this only settles the conflict.
    bool leaving = file_.conflict() && file_.dirty(editor_.version());
    std::string err, name = file_.keep_beside(leaving ? text_to_save() : std::string(), editor_.version(), &err);
    if (!leaving) return;
    if (name.empty()) {
        status("couldn't keep your edits: " + err, true);
        return;
    }
    status("kept your edits as " + name + "; " + note_->name + " has the version from disk");
}

void App::open_note(const NoteRef& n, bool push_history) {
    std::optional<Jump> jump;
    if (push_history) jump.emplace(*this);
    close_bar();
    keep_yours();
    save();
    remember_view();
    bool config = n.path == cfg_path_, visit = n.visit;
    std::string text;
    if (n.path == kManual) {
        text.assign((const char*)g_manual, g_manual_size);
        file_.close();
    } else if (visit) {
        // Read, never written: no file of its own to keep in step with.
        if (!read_file(n.path, text)) {
            status("couldn't open " + n.name + "; it may have been moved, deleted or locked by another app", true);
            return;
        }
        if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // a byte order mark isn't text
        file_.close();
    } else if (!file_.load(n.path, text, nullptr)) {
        status("couldn't open " + n.name + "; it may have been moved, deleted or locked by another app", true);
        return;
    }

    // The config is edited like a note, but as ini text and never in reader
    // mode; a visiting doc (the manual too) is only ever in reader mode. The
    // reader preference comes back when you return to notes.
    bool was_fixed = is_config() || is_visiting();
    if ((config || visit) && !was_fixed) reader_pref_ = editor_.reader();
    if (config || visit) editor_.set_reader(visit, false);
    else if (was_fixed) editor_.set_reader(reader_pref_, false);
    editor_.set_syntax(config ? Syntax::Ini : Syntax::Markdown);
    images_.set_note_dir(n.path.parent_path());  // images are found from the note's (or doc's) folder
    editor_.set_text(text);
    note_ = n;
    if (!config && !visit) {
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
    file_.synced(editor_.version());
    seen_version_ = editor_.version();
    if (auto it = views_.find(path_u8(n.path)); it != views_.end()) editor_.restore_view(it->second.first, it->second.second);
    fade_start_ = anim::clock;
    if (!config && !visit) recent_.visit(n);
    set_title();
}

bool App::need_note() {
    if (is_config()) status("open a note first; the config stays where it is");
    else if (is_manual()) status("open a note first; the manual is read only");
    else if (is_visiting()) status("keep it as a note first; this doc is only visiting");
    return is_note();
}

void App::open_manual() {
    NoteRef r;
    r.space = "delta";
    r.name = "manual";
    r.path = kManual;
    r.visit = true;
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
    space_count_ = -1;
    fade_start_ = anim::clock;
    set_title();
}

void App::switch_space(const std::string& space) {
    if (space == space_ && note_) return;
    if (auto e = store_.ensure_space(space); !e.empty()) { status(e, true); return; }
    space_count_ = -1;
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
    space_count_ = -1;
    open_note(n);
    editor_.restore_view({(int)editor_.lines().size() - 1, 0}, 0);
    status("created " + name);
}

void App::open_daily() {
    std::tm tm = local_tm(std::time(nullptr));
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
    if (link.tag) { open_palette("#" + link.target + " "); return; }  // the notes that have it
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

// Rename: the note's name in the status line becomes editable where it sits,
// after the space.
void App::rename_note() {
    if (!need_note()) return;
    BarPrompt p;
    p.lead = {{note_->space, &Theme::dim}, {" / ", &Theme::faint}};
    p.note = true;
    p.problem = [this](const std::string& v) -> std::string {
        if (v == note_->name) return {};
        if (auto e = Store::check_name(v, "note"); !e.empty()) return e;
        if (store_.exists(note_->space, v) && !iequals(v, note_->name)) return "there's already a note called " + v;
        return {};
    };
    p.hint = [](const std::string&) { return std::string("enter to rename"); };
    p.submit = [this](const std::string& v) -> std::string {
        if (v == note_->name) return {};
        save();
        std::string old = path_u8(note_->path);
        NoteRef n = *note_;
        if (auto e = store_.rename_note(n, v); !e.empty()) return e;
        note_relocated(old, n);
        set_title();
        status("renamed to " + n.name);
        return {};
    };
    open_prompt(std::move(p), note_->name);
}

void App::move_note() {
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
            space_count_ = -1;
            note_relocated(old, n);
            space_ = sp;
            apply_spelling();  // this space may want other dictionaries
            status("moved to " + sp);
        };
        items.push_back(it);
    }
    if (items.empty()) { status("no other space yet; make one with new space"); return; }
    palette_.pick("move to: ", items);
}

void App::note_relocated(const std::string& old_path, const NoteRef& n) {
    views_.erase(old_path);
    auto relocated = [&](NoteRef& r) { if (path_u8(r.path) == old_path) r = n; };
    history_.update(relocated);
    recent_.update(relocated);
    note_ = n;
    file_.moved(n.path);
}

void App::delete_note() {
    if (!need_note()) return;
    NoteRef n = *note_;
    confirm("delete? ", "delete \"" + n.name + "\"", "to .trash", [this, n] {
        save();
        if (auto e = store_.trash_note(n); !e.empty()) { status(e, true); return; }
        space_count_ = -1;
        views_.erase(path_u8(n.path));
        history_.forget(n.path);
        recent_.forget(n.path);
        note_.reset();
        auto rest = store_.notes(n.space);
        if (!rest.empty()) open_note(rest.front(), false);
        else close_note();
        status("deleted " + n.name + " (it's in .trash)");
    });
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
            file_.moved(note_->path);
        }
        status("renamed space to " + to);
    });
}

void App::delete_space() {
    std::string sp = space_;
    if (store_.note_count(sp) > 0) { status(quote(sp) + " still has notes; move or delete them first", true); return; }
    confirm("delete space? ", "delete space \"" + sp + "\"", "", [this, sp] {
        close_note();
        if (auto e = store_.delete_space(sp); !e.empty()) { status(e, true); return; }
        std::string next = cfg_.default_space == sp ? "" : cfg_.default_space;
        auto spaces = store_.spaces();
        if (next.empty() && !spaces.empty()) next = spaces.front();
        if (next.empty()) next = "default";
        switch_space(next);
        status("deleted space " + sp);
    });
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
            space_count_ = -1;
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
    cache_counts_.clear();
    cache_text_.clear();
    cache_order_.gen = -1;
    links_gen_ = -1;
    // Pressing the same shortcut again closes it; the other scope's
    // shortcut switches scope instead.
    if (palette_.in_mode(prefix.empty() ? 0 : prefix[0])) {
        if (palette_.scoped() && palette_.wide() != wide) palette_.set_wide(wide);
        else palette_.close();
        return;
    }
    palette_.open(prefix, wide);
}

void App::confirm(const std::string& prompt, const std::string& yes_label, const std::string& yes_meta,
                  std::function<void()> action) {
    PaletteItem yes, no;
    yes.label = yes_label;
    yes.meta = yes_meta;
    yes.emphasis = true;
    yes.action = std::move(action);
    no.label = "keep it";
    no.action = [] {};
    palette_.pick(prompt, {yes, no});
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
    int lead = first == std::string::npos ? 0 : (int)first;
    for (int k = 0; k < (int)len; k++) it.hl.push_back(hl0 - lead + k);
    return it;
}

// A note's lines, read once while the palette is open.
const App::NoteText& App::note_lines(const NoteRef& n) {
    std::string key = path_u8(n.path);
    auto it = cache_text_.find(key);
    if (it == cache_text_.end()) {
        std::string text;
        read_file(n.path, text);
        NoteText t;
        t.lines = split_lines(text);
        t.lower.reserve(t.lines.size());
        for (auto& l : t.lines) t.lower.push_back(lower(l));
        it = cache_text_.emplace(std::move(key), std::move(t)).first;
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
    // The keys of the mode's own commands (see Scope).
    if (prefixed && std::strchr(Palette::kScoped, c)) {
        std::string sort_key = c == '.' ? key_hint("palette.sort", "sort") : "";
        std::string widen = key_hint("palette.widen", "widen");
        palette_.set_hint(sort_key + (sort_key.empty() || widen.empty() ? "" : "  ") + widen);
    }

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
        // Sorted once for what's listed; typing only filters it.
        NotesOrder& ord = cache_order_;
        if (ord.gen != palette_.generation() || ord.all != all || ord.recent != sort_recent_ || !iequals(ord.only, only)) {
            ord.gen = palette_.generation();
            ord.all = all;
            ord.recent = sort_recent_;
            ord.only = only;
            std::vector<NoteRef> listed;
            for (auto& n : all ? cache_all_ : cache_notes_)
                if (only.empty() || iequals(n.space, only)) listed.push_back(n);
            // By name (as listed, so every space's are grouped by space), or by
            // recent: the notes you've had open, latest first, then the rest by
            // when they were changed. Matches keep this order among equals.
            std::vector<size_t> by(listed.size());
            for (size_t i = 0; i < by.size(); i++) by[i] = i;
            if (sort_recent_) {
                std::map<std::string, int> rank;
                for (auto& n : recent_.list()) rank.emplace(path_u8(n.path), (int)rank.size());
                std::vector<int> ranks;
                for (auto& n : listed) {
                    auto it = rank.find(path_u8(n.path));
                    ranks.push_back(it == rank.end() ? INT_MAX : it->second);
                }
                std::stable_sort(by.begin(), by.end(), [&](size_t a, size_t b) {
                    return ranks[a] != ranks[b] ? ranks[a] < ranks[b] : listed[a].mtime > listed[b].mtime;
                });
            } else {
                std::vector<std::string> labels;
                for (auto& n : listed) labels.push_back(all ? n.space + "/" + n.name : n.name);
                std::sort(by.begin(), by.end(), [&](size_t a, size_t b) { return natural_less(labels[a], labels[b]); });
            }
            ord.notes.clear();
            for (size_t i : by) ord.notes.push_back(std::move(listed[i]));
        }
        const std::vector<NoteRef>& notes = ord.notes;
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
            // Nothing to rank by: the first ones are the ones shown.
            if (name.empty() && out.size() >= 300) break;
        }
        sort();
        if (out.size() > 300) out.resize(300);  // the best, once ranked
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
        if (cache_counts_.size() != cache_spaces_.size()) {
            cache_counts_.clear();
            for (auto& sp : cache_spaces_) cache_counts_.push_back(store_.note_count(sp));
        }
        for (size_t k = 0; k < cache_spaces_.size(); k++) {
            const std::string& sp = cache_spaces_[k];
            PaletteItem it;
            it.label = sp;
            if (!fuzzy_match(q, sp, it.score, &it.hl)) continue;
            if (iequals(sp, q)) exact = true;
            int count = cache_counts_[k];
            it.meta = sp == space_ ? "current" : count_of(count, "note");
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
        // `low` is the lines lowercased, when they're cached that way.
        auto scan = [&](const NoteRef& n, const std::vector<std::string>& lines, const std::vector<std::string>* low) {
            for (int i = 0; i < (int)lines.size() && out.size() < 200; i++) {
                size_t at = low ? (*low)[i].find(lq) : lower(lines[i]).find(lq);
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
        if (is_note()) scan(*note_, editor_.lines(), nullptr);
        if (palette_.wide() && cache_all_.empty()) cache_all_ = store_.all_notes();
        for (auto& n : palette_.wide() ? cache_all_ : cache_notes_) {
            if (out.size() >= 200) break;
            if (note_ && n.path == note_->path) continue;
            const NoteText& t = note_lines(n);
            scan(n, t.lines, &t.lower);
        }
        palette_.set_empty(palette_.wide() ? "no matches for " + quote(q) + " in any space"
                                           : "no matches for " + quote(q) + " in " + space_ + "; tab searches every space");
        return;
    }

    if (c == '#') {
        build_tags(text.substr(1), out, total);  // untrimmed: a space finishes a tag
        return;
    }

    // Links here: every [[link]] in any space that leads to the open note.
    if (c == '[') {
        if (!is_note()) { palette_.set_empty("open a note first; its links are listed here"); return; }
        // Found once as the palette opens; typing only filters them.
        if (links_gen_ != palette_.generation()) {
            links_gen_ = palette_.generation();
            cache_links_.clear();
            if (cache_all_.empty()) cache_all_ = store_.all_notes();
            for (auto& n : cache_all_) {
                if (n.path == note_->path) continue;
                const auto& lines = note_lines(n).lines;
                for (auto& h : links_to(lines, n.space, note_->space, note_->name, note_index_)) {
                    LinkHit hit;
                    hit.note = n;
                    hit.a = {h.line, h.col0};
                    hit.b = {h.line, h.col1};
                    hit.item = line_item(lines[h.line], h.col0, h.col1 - h.col0);
                    hit.item.meta = (n.space == space_ ? "" : n.space + "/") + n.name + ":" + std::to_string(h.line + 1);
                    hit.match = hit.item.label + " " + hit.item.meta;
                    cache_links_.push_back(std::move(hit));
                }
            }
        }
        total = (int)cache_links_.size();
        for (auto& hit : cache_links_) {
            PaletteItem it = hit.item;
            if (!fuzzy_match(q, hit.match, it.score, nullptr)) continue;
            it.action = [this, n = hit.note, a = hit.a, b = hit.b] {
                Jump j(*this);
                open_note(n);
                editor_.select_range(a, b);
            };
            out.push_back(std::move(it));
        }
        palette_.set_empty(total == 0 ? "no notes link to " + note_->name + " yet; link one with [[" + note_->name + "]]"
                                      : "no links match " + quote(q));
        sort();
        return;
    }

    if (!prefixed) {
        palette_.set_empty("no command matches " + quote(q));
        total = (int)cmds_.size();
        size_t prefixed_w = 0;
        for (auto& cmd : cmds_)
            if (cmd.prefix && !cmd.chords.empty()) prefixed_w = std::max(prefixed_w, chord_str(cmd.chords.front()).size());
        for (auto& cmd : cmds_) {
            PaletteItem it;
            it.label = cmd.title;
            if (!match_command(q, cmd.title, cmd.id, command_keywords(cmd.id), it.score, &it.hl, &it.aside)) continue;
            if (cmd.scope == Scope::Tag && !tag_at_caret()) it.score -= 50;  // "rename" is the note's, off a tag
            if (cmd.scope == Scope::Slip && !slip_at_caret()) it.score -= 50;
            if (cmd.scope == Scope::Visit && !(is_visiting() && !is_manual())) it.score -= 50;  // "keep" is for visitors
            // Prefixes sit in a column of their own: the palette is mono, so
            // padding the keys to the widest a prefixed command has lines them up.
            std::string keys = cmd.chords.empty() ? "" : chord_str(cmd.chords.front());
            if (cmd.prefix) keys = std::string(1, cmd.prefix) + "  " + std::string(prefixed_w - std::min(prefixed_w, keys.size()), ' ') + keys;
            it.meta = keys;
            it.action = cmd.run;
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
    if (note_ && !file_.conflict() && file_.dirty(editor_.version())) t = std::min(t, last_change_ + cfg_.autosave_delay);
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
    space_count_ = -1;  // notes made or removed elsewhere, for the empty page
    if (!note_) return;
    // Notes made, renamed or removed (here or elsewhere) change which links
    // lead somewhere. Not while away: coming back checks at once (on_focus).
    if (!is_config() && focused_) {
        NoteIndex ix = store_.index();
        if (!(ix == note_index_)) {
            note_index_ = std::move(ix);
            hints_version_ = UINT64_MAX;
        }
    }
    // Rewritten under unsaved edits, it's a conflict, which the status line
    // shows until it's settled.
    if (file_.poll(editor_.version()) == NoteFile::Poll::Changed) reload_note();
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
    // The innermost scope that binds the key; among equals, the first listed.
    Command* hit = nullptr;
    for (auto& c : cmds_) {
        if (hit && depth(c.scope) <= depth(hit->scope)) continue;
        bool pressed = std::any_of(c.chords.begin(), c.chords.end(), [&](const Chord& ch) { return chord_pressed(ch, c.repeat); });
        if (pressed && in_scope(c.scope)) hit = &c;
    }
    if (hit) {
        hit->run();
        fired = true;
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
    if (space_count_ < 0) space_count_ = store_.note_count(space_);
    int n = space_count_;
    std::string sub = space_ + "/ is empty";
    if (n > 0) sub = space_ + "/ has " + count_of(n, "note") + ", none open";

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
        if (file_.dirty(editor_.version())) {
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
            count(n == 0 ? "no problems" : count_of(n, "problem"), n > 0, T.error, [this] { goto_problem(1); });
        int m = (int)spelling_.size() - (shown_held_.line >= 0 ? 1 : 0);  // not the word being typed
        if (m && !is_config())
            count(count_of(m, "misspelling"), true, T.spelling, [this] { goto_misspelling(1); });
        if (!is_config()) {
            int w = editor_.word_count();
            put_right(count_of(w, "word"), T.dim);
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
    if (status_.empty() && file_.conflict()) {
        std::string save = chord_for("note.save");
        centred(fit("changed on disk; " + (save.empty() ? "\"save note now\"" : save) +
                    " keeps your version, \"reload note\" takes the file's"),
                T.bright);
    }

    // Otherwise, in the same place, the echo of what the caret is on.
    if (!echo_.empty()) echo_shown_ = echo_;
    echo_a_ = anim::approach(echo_a_, echo_.empty() ? 0.0f : 1.0f, 16.0f, ImGui::GetIO().DeltaTime, 0.004f);
    if (status_.empty() && !file_.conflict() && echo_a_ > 0.0f && !echo_shown_.empty()) {
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
        if (act == Echo::Act::FixSlip) fix_slip();
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

    run_pending(now);

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
                if (file_.dirty(editor_.version())) {
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
            if (int i = misspelling_at(spelling_, editor_.position_at(io.MousePos)); i >= 0) {
                editor_.jump_to({spelling_[i].line, spelling_[i].col0});
                fix_spelling();
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
            case '#': return "tags " + (palette_.wide() ? std::string("all") : space_) + ": #";
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
        if (!file_.conflict() && file_.dirty(editor_.version()) && now - last_change_ >= cfg_.autosave_delay) save();
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
