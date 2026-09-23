#pragma once
#include <ctime>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
#include "keys.h"
#include "palette.h"
#include "store.h"

struct GLFWwindow;

struct AppOptions {
    fs::path config_path;               // empty = default location
    std::vector<std::string> run;       // command ids to run after start-up
};

class App {
public:
    bool init(GLFWwindow* window, const AppOptions& opts);
    void frame();
    void shutdown();

    bool should_quit() const { return quit_; }
    double next_wake(double now);       // when the idle loop must render next
    ImU32 background() const;
    void set_dpi(float dpi) { dpi_ = dpi; }
    void on_focus(bool focused);

private:
    struct Command {
        std::string id, title, defaults;
        std::function<void()> run;
        bool repeat = false;     // fires on key repeat (line moves, undo...)
        bool global = false;     // also works while the palette is open
        bool in_palette = true;
        std::vector<Chord> chords;
    };

    // setup
    void register_commands();
    void load_settings();
    void apply_bindings();
    void check_settings(const Config& c, std::vector<ConfigIssue>& issues) const;
    void set_problems(std::vector<ConfigIssue> issues);
    void show_problems(double now);
    void goto_problem(int dir);
    void open_config();
    void add_setting();
    bool is_config() const { return note_ && note_->path == cfg_path_; }
    std::vector<BindingDoc> binding_docs() const;
    Command* find_command(const std::string& id);
    std::string chord_for(const std::string& id) const;

    // notes
    void open_note(const NoteRef& n, bool push_history = true);
    void close_note();
    bool save();
    void remember_view();
    void switch_space(const std::string& space);
    void create_note(const std::string& space, const std::string& name);
    void open_daily();
    void open_link(const LinkSpan& link);
    void open_by_name(std::string target);
    void rename_note();
    void move_note();
    void delete_note();
    void new_space();
    void rename_space();
    void delete_space();
    void history_step(int dir);
    void set_title();

    // ui
    void dispatch_keys(bool& fired);
    void build_palette(const std::string& text, std::vector<PaletteItem>& out, int& total);
    void open_palette(const std::string& prefix);
    void draw_status(ImVec2 min, ImVec2 max);
    void draw_empty(ImVec2 min, ImVec2 max, float page);
    void open_find(bool replace);
    void close_find();
    void draw_find_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    static int find_input_callback(ImGuiInputTextCallbackData* d);
    void status(const std::string& msg, bool error = false);
    void poll_files(double now);
    float base_size() const;
    bool editing() const { return note_.has_value() && !editor_.reader(); }
    void toggle_fullscreen();

    GLFWwindow* win_ = nullptr;
    Config cfg_;
    fs::path cfg_path_, state_path_;
    fs::file_time_type cfg_mtime_{};
    // The config's problems, in line order: from the file, or checked live
    // from the text while it's open in delta.
    std::vector<ConfigIssue> config_problems_;
    double next_problem_check_ = 0;           // a held-back problem shows then
    uint64_t hints_version_ = UINT64_MAX;     // editor version the hints and problems match
    bool reader_pref_ = false;                // reader mode to restore after the config
    Store store_;
    Editor editor_;
    Palette palette_;
    std::vector<Command> cmds_;
    std::vector<std::string> pending_run_;
    double run_after_ = 0;             // --run steps wait until this time
    ImGuiKey release_key_ = ImGuiKey_None;  // scripted key press to release next frame

    ImFont* sans_ = nullptr;
    ImFont* mono_ = nullptr;
    ImFont* sans_bold_ = nullptr;  // SemiBold weights
    ImFont* mono_bold_ = nullptr;
    float dpi_ = 1, zoom_ = 1;

    std::string space_;
    std::optional<NoteRef> note_;
    bool crlf_ = false;
    uint64_t saved_version_ = 0, seen_version_ = 0;
    double last_change_ = 0;
    fs::file_time_type disk_mtime_{};
    double next_poll_ = 0;
    bool focused_ = true;

    std::map<std::string, std::pair<TextPos, float>> views_;
    std::vector<NoteRef> history_;
    int hist_pos_ = -1;

    // palette caches, refreshed each time it opens
    std::vector<NoteRef> cache_notes_, cache_all_;
    std::vector<std::string> cache_spaces_;
    std::map<std::string, std::vector<std::string>> cache_text_;

    std::string status_;
    bool status_error_ = false;
    double status_time_ = -100;
    double fade_start_ = -100;

    bool fullscreen_ = false;
    int win_x_ = 0, win_y_ = 0, win_w_ = 0, win_h_ = 0;
    bool quit_ = false;

    // Find bar (in the status line).
    bool find_open_ = false;
    bool find_replace_ = false;       // replace field shown
    int find_field_ = 1;              // 1 find, 2 replace: which has focus
    bool find_focus_ = false;
    bool find_select_all_ = false;
    int find_select_pending_ = 0;     // 1 select all, 2 cursor to end
    char find_buf_[256] = {};
    char repl_buf_[256] = {};
    std::string find_last_;
};
