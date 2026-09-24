#pragma once
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
#include "fonts.h"
#include "keys.h"
#include "image_cache.h"
#include "images.h"
#include "palette.h"
#include "speller.h"
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
    // Files dropped on the window (UTF-8 paths), at a point in the window.
    void on_drop(std::vector<std::string> paths, ImVec2 at) { drops_.push_back({std::move(paths), at}); }
    double next_wake(double now);       // when the idle loop must render next
    ImU32 background() const;
    void set_dpi(float dpi) { dpi_ = dpi; }
    void on_focus(bool focused);
    // Loads the fonts the config picks, if they aren't the ones loaded.
    // Only between frames: the old fonts go.
    void apply_fonts();

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
    void refresh_problems();
    void apply_spelling(bool force = false);
    void check_spelling();
    TextPos spell_held();
    void show_problems(double now);
    void update_echo();
    const Diagnostic* misspelt_at_caret() const;
    void goto_misspelling(int dir);
    void fix_spelling();
    void quick_fix();
    void quick_apply();
    void fix_load(const Diagnostic& d);
    void fix_field(const std::string& text);
    void fix_next(TextPos from);
    void add_to_dictionary(const std::string& word);
    void goto_problem(int dir);
    void open_config();
    void add_setting();
    void publish_problems(bool announce);
    ImFont* load_font(const FontBytes& bytes, int index, const unsigned char* noto, std::size_t noto_size);
    void install_fonts(FontLoader::Pair& in);
    std::vector<PaletteItem> font_rows(bool mono);
    bool font_picker_open() const { return font_picker_ && palette_.visible() && palette_.generation() == font_picker_gen_; }
    void choose_font(bool mono);
    void set_font(bool mono, const std::string& value);
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

    // images
    void paste(bool plain);
    void copy_out(bool cut);
    std::string import_image(const std::string& bytes, std::string& err);  // -> the line for it, "" if not
    std::vector<std::string> import_files(const std::vector<fs::path>& files, int& skipped);
    static std::string as_row(const std::vector<std::string>& lines);
    void fetch_image(const std::string& url);
    void poll_downloads();
    void handle_drops();
    void open_viewer(int line, int tile);
    void draw_viewer();

    // ui
    void dispatch_keys(bool& fired);
    void build_palette(const std::string& text, std::vector<PaletteItem>& out, int& total);
    void open_palette(const std::string& prefix);
    void draw_status(ImVec2 min, ImVec2 max);
    void draw_empty(ImVec2 min, ImVec2 max, float col_x, float column, float top_pad, float page);
    // The status line as a minibuffer: tools that act on the note in view
    // happen down there, so the note stays in sight. (Choosing from a list,
    // headings included, stays in the palette.)
    enum class Bar { None, Find, Line, Rename, Fix };
    void open_bar(Bar kind);
    void close_bar();                   // keeps where the tool left you
    void cancel_bar();                  // escape: jumps go back to where you were
    void draw_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    bool bar_input(const char* id, char* buf, size_t size, int which, float x, float y, float w);
    static int bar_input_callback(ImGuiInputTextCallbackData* d);
    void open_find(bool replace);
    void close_find() { if (bar_ == Bar::Find) close_bar(); }
    void draw_find_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    void open_line(const std::string& text = "");
    void draw_line_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    void draw_rename_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    void draw_fix_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty);
    void status(const std::string& msg, bool error = false);
    void poll_files(double now);
    float base_size() const;
    bool editing() const { return note_.has_value() && !editor_.reader(); }
    void toggle_fullscreen();

    GLFWwindow* win_ = nullptr;
    Config cfg_;
    fs::path cfg_path_, state_path_;
    fs::file_time_type cfg_mtime_{};
    // The config's problems: from the file, or checked live from the text
    // while it's open in delta.
    std::vector<ConfigIssue> config_problems_;
    // The open document's problems, in line order (see refresh_problems).
    std::vector<Diagnostic> problems_;
    Speller speller_;
    fs::path dict_dir_, words_path_;          // <config dir>/dictionaries, <config dir>/words.txt
    fs::file_time_type words_mtime_{};
    std::string spell_key_ = "-";             // the dictionaries asked for ("" none)
    bool spell_report_ = false;               // say what didn't load, once it's loaded
    std::vector<Diagnostic> spelling_;        // the open note's misspelt words
    uint64_t spell_version_ = UINT64_MAX;     // editor version spelling_ matches
    uint64_t spell_gen_ = UINT64_MAX;         // ...and speller generation
    bool spell_pending_ = false;              // lines left to check next frame
    uint64_t edit_version_ = 0;               // editor version at the last edit seen
    TextPos spell_hold_{-1, 0};               // where the last edit left the caret (line -1: none)
    uint64_t diag_gen_ = 0;                   // bumped when problems_ or spelling_ change
    uint64_t shown_gen_ = UINT64_MAX;         // ...as last handed to the editor
    int shown_hold_ = -2;                     // the line held back then
    TextPos shown_held_{-2, 0};               // and the word
    // The status line's echo (see update_echo): what it says, and what's
    // showing (kept while it fades out). A part can be clicked to act.
    enum class Tone : uint8_t { Plain, Strong, Quiet };  // message, the words that matter, asides
    struct Echo {
        std::string text;
        Tone tone = Tone::Plain;
        enum class Act : uint8_t { None, QuickFix, FixSpelling } act = Act::None;
    };
    std::vector<Echo> echo_, echo_shown_;
    float echo_a_ = 0;
    NoteIndex note_index_;                    // where [[links]] can lead
    double next_problem_check_ = 0;           // a held-back problem shows then
    uint64_t hints_version_ = UINT64_MAX;     // editor version the hints and problems match
    bool reader_pref_ = false;                // reader mode to restore after the config
    Store store_;
    Editor editor_;
    Palette palette_;
    ImageCache images_;       // what the editor draws images with
    ImageStore image_store_;  // where they're kept: <notes>/.delta/images

    struct Drop { std::vector<std::string> paths; ImVec2 at; };
    std::vector<Drop> drops_;

    // An image URL being fetched. Its line reads ![](delta-pending:<id>) until
    // it lands (then it's the stored image) or fails (then it's the URL).
    struct Download {
        int id = 0;
        std::string url, marker;
        fs::path note;               // the note it was pasted into
        std::mutex mu;
        bool done = false, ok = false;
        std::string bytes, err;
    };
    std::vector<std::shared_ptr<Download>> downloads_;
    int next_download_ = 1;

    // Full-window image viewer.
    struct Viewer {
        bool open = false, closing = false;
        std::string src;
        int line = -1, tile = 0;     // where it is in the note (to zoom back to)
        ImVec2 from_a, from_b;       // its rect in the note when opened
        float t = 0;                 // 0 = in the note, 1 = full window
        float zoom = 1;              // on top of fitting the window (as shown)
        ImVec2 pan{0, 0};
        float zoom_to = 1;           // where input has sent them; the shown
        ImVec2 pan_to{0, 0};         // values ease there
        bool pressed = false, dragged = false;
        ImVec2 press_at;
        int opened_frame = -1;       // the key or click that opened it isn't also a close
    } viewer_;
    std::vector<Command> cmds_;
    std::vector<std::string> pending_run_;
    double run_after_ = 0;             // --run steps wait until this time
    ImGuiKey release_key_ = ImGuiKey_None;  // scripted key press to release next frame

    ImFont* sans_ = nullptr;
    ImFont* mono_ = nullptr;
    ImFont* sans_bold_ = nullptr;  // SemiBold weights
    ImFont* mono_bold_ = nullptr;
    FontLoader font_loader_;
    std::optional<FontLoader::Pair> fonts_in_;  // found at start-up, to load before the first frame
    std::string fonts_key_;                     // which faces are loaded
    std::vector<FontBytes> font_bytes_;         // theirs, held while they're loaded
    // What was wrong with each font setting ("font", "mono_font"), and the
    // value it was wrong about.
    struct FontProblem { std::string value, problem; };
    std::map<std::string, FontProblem> font_problems_;
    std::vector<ConfigIssue> config_issues_;    // the rest of the config's problems
    std::vector<FontFamily> families_;          // installed, as last listed
    int font_picker_ = 0;                       // 1 font, 2 mono font: that picker is open
    int font_picker_gen_ = 0;                   // ...as the palette's this generation
    // The font picker's preview: the font wanted ("s:" or "m:", for font or
    // mono_font, then the setting's value; "" for none), asked the font
    // worker for, and loaded (or found wrong: why). The selection must rest a
    // moment before a font is wanted, so scrolling past doesn't load each one.
    std::string preview_want_, preview_asked_, preview_loaded_, preview_error_;
    ImFont* preview_font_ = nullptr;
    FontBytes preview_bytes_;
    std::string preview_sel_;
    double preview_since_ = 0;
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

    // Status-line bar.
    Bar bar_ = Bar::None;
    int bar_field_ = 1;               // which field has focus (find: 1 find, 2 replace)
    bool bar_focus_ = false;          // focus bar_field_ this frame
    bool bar_select_all_ = false;     // ...with its text selected
    int bar_select_pending_ = 0;      // 1 select all, 2 cursor to end
    char bar_buf_[256] = {};          // line, rename and fix text
    std::string bar_set_;             // text for the field being edited, set on its next callback
    bool bar_set_pending_ = false;
    std::string bar_last_;            // bar_buf_ as last acted on
    std::string bar_error_;           // why the last Enter didn't go through
    TextPos bar_cur_, bar_anchor_;    // where you were, for escape
    // Fix: the misspelt word being fixed (as written, where), its suggestions
    // and which one is in the field; where the pass began and whether it has
    // wrapped past the end; and, after a fix, where to look for the next.
    struct Fix {
        int line = -1, col0 = 0, col1 = 0;
        std::string word;
        std::vector<std::string> sugg;
        int pick = 0;
    } fix_;
    TextPos fix_origin_, fix_from_;
    // The quick fix (alt+.): the word it changed (where, as written), the
    // guesses it goes through and which is in, where the caret was, and the
    // caret and editor version it left (pressed again with those unchanged,
    // it goes on to the next guess).
    struct Quick {
        int line = -1, col0 = 0;
        std::string word;
        std::vector<std::string> options;
        int pick = 0;
        TextPos from, caret;
        uint64_t version = 0;
    } quick_;
    bool fix_wrapped_ = false, fix_advance_ = false;
    float bar_scroll_ = 0;

    bool find_replace_ = false;       // replace field shown
    char find_buf_[256] = {};
    char repl_buf_[256] = {};
    std::string find_last_;
};
