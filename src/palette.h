#pragma once
// The command palette. One text box, three kinds of use:
//   query  - the app fills the list from the text (a prefix picks the mode)
//   pick   - a fixed list the text filters
//   prompt - the text itself is the answer (new note name, rename...)
// It's typeset like a terminal picker: mono, a prompt naming the mode, a
// match count, and a > marker (or an inverted row) for the selection.
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "imgui.h"
#include "smear.h"

struct PaletteItem {
    std::string label;
    std::string meta;           // right-aligned, dim
    std::vector<int> hl;        // byte offsets in label to highlight
    std::function<void()> action;
    int score = 0;
    int indent = 0;             // heading depth etc.
    int dim = 0;                // leading bytes of label drawn dim (a note's "space/")
    std::string aside;          // faint, after the label (the keyword a command matched by)
    bool emphasis = false;      // "+ new ..." rows
    bool current = false;       // where you are: selected when the list is built
    // Taking the row types this into the query instead of closing (a tag
    // finished, for the notes that have it). A query's text, mode included.
    std::string fill;
};

class Palette {
public:
    // Fills `out` for the full query text (mode prefix included) and sets
    // `total` to how many candidates there were before filtering (-1: none).
    using Builder = std::function<void(const std::string& text, std::vector<PaletteItem>& out, int& total)>;
    // The prompt shown for a mode prefix (0 = no prefix).
    using PromptFor = std::function<std::string(char mode)>;

    // Characters that switch mode when typed first. No prefix is commands,
    // the root that every other mode backspaces out to.
    static constexpr const char* kModes = ".@:?/[#";
    // Modes with a scope, which set_wide widens from this space to every
    // space (notes, text search and tags). App binds it to tab.
    static constexpr const char* kScoped = ".?#";

    // Extras a pick can have.
    struct PickOptions {
        // A row for the text as typed, above the matches (a file's path,
        // say). False: none.
        std::function<bool(const std::string& text, PaletteItem& row)> typed;
        // Draws a line under the list about the selected row (null when
        // nothing is), `h` high at `pos` and `w` wide. Every frame the
        // palette shows.
        std::function<void(const PaletteItem* sel, ImDrawList* dl, ImVec2 pos, float w, float h, float alpha)> preview;
        // Told the selected row (null when nothing is) at the end of every
        // frame the pick is open.
        std::function<void(const PaletteItem* sel)> select;
    };

    // `wide`: a scoped mode starts on every space.
    void open(const std::string& text, bool wide = false);
    void pick(const std::string& prompt, std::vector<PaletteItem> items, PickOptions opts = {});
    // A pick held open by ctrl, like alt+tab: tab and shift+tab move the
    // selection, and letting go of ctrl takes it. Row `sel` starts selected.
    void hold(const std::string& prompt, std::vector<PaletteItem> items, int sel);
    void prompt(const std::string& prompt, const std::string& initial, const std::string& action_label,
                std::function<void(const std::string&)> submit);
    // A pick's list, changed while it's open. The query stays, and so does
    // the selection if its row is still there.
    void update_items(std::vector<PaletteItem> items);
    void close();
    bool is_open() const { return open_; }
    // Changes each time it opens, to tell one list from the next.
    int generation() const { return generation_; }
    bool visible() const { return open_ || close_t_ < 1.0f; }
    std::string text() const { return mode_ ? std::string(1, mode_) + buf_ : std::string(buf_); }
    bool in_mode(char m) const { return open_ && kind_ == Kind::Query && mode_ == m; }
    bool scoped() const { return kind_ == Kind::Query && mode_ && std::strchr(kScoped, mode_); }
    // Every space rather than this one (see kScoped).
    bool wide() const { return wide_ && scoped(); }
    void set_wide(bool w);
    // A word beside the count about how the list is shown ("by name"),
    // set by the builder; empty for none.
    void set_tag(std::string t) { tag_ = std::move(t); }
    // The keys of the mode's own commands ("ctrl+r to sort"), shown before the
    // tag until something is typed. Set by the builder; empty for none.
    void set_hint(std::string t) { hint_ = std::move(t); }
    // What an empty list says instead of "no matches": why it's empty and
    // where to go from there. Set by the builder; empty for the default.
    void set_empty(std::string t) { empty_ = std::move(t); }
    // Builds the list again next frame (something it's built from changed).
    void rebuild() { built_for_ = "\x01"; }

    enum class Selection { Marker, Invert };

    struct Frame {
        ImFont* mono;
        ImFont* mono_bold;   // matched characters
        float size;          // body font, ImGui units
        float text;          // its own (mono) text, ImGui units
        float dpi;
        float col_x, col_w;  // the text column it's set in
        float top, bottom;   // first text line of the page / lowest usable y
        double now;
        float dt;
        Selection selection;
        float caret_w;       // matches the editor's caret
        bool blink;
    };
    // How visible the page behind should be. The page and palette take
    // turns (one fades out before the other fades in), so they never overlap.
    float page_alpha() const { return page_a_; }

    // Returns the chosen action (run it after the frame's UI is done).
    std::function<void()> draw(const Frame& f, const Builder& build, const PromptFor& prompt_for);

private:
    enum class Kind { Query, Pick, Prompt };
    void set_text(const std::string& t);
    void commit(int row);  // close after a pick: the chosen row lingers a beat
    static int input_callback(ImGuiInputTextCallbackData* d);

    Kind kind_ = Kind::Query;
    bool open_ = false;
    bool hold_ = false;          // see hold()
    double open_time_ = -10;
    int open_frame_ = -1;        // the frame it opened in: the key or click that opened it doesn't also act on it
    float close_t_ = 1.0f;       // 0..1 progress of the close fade
    char mode_ = 0;              // query mode prefix, kept out of the text box
    bool wide_ = false;          // see wide()
    char buf_[512] = {};
    std::string pending_text_;
    bool apply_text_ = false;
    bool focus_ = false;
    std::string title_, action_label_, tag_, hint_, empty_;
    std::vector<PaletteItem> fixed_, items_;
    PickOptions opts_;
    int generation_ = 0;
    std::string keep_sel_;             // after update_items: the row to stay on
    int total_ = -1;
    std::function<void(const std::string&)> submit_;
    std::string built_for_;
    int sel_ = 0;
    bool reveal_sel_ = false;          // scroll the selection into view once
    float sel_y_ = 0, scroll_ = 0, scroll_target_ = 0;
    bool snap_ = true;
    float page_a_ = 1;
    SmearRect bar_;                    // invert bar, moving like the caret
    SmearRect caret_;                  // our own caret (ImGui's is hidden)
    int cursor_ = 0;                   // caret byte offset, from the input callback
    int last_cursor_ = -1;
    std::string last_buf_;
    double caret_activity_ = 0;        // restarts the blink
    bool committed_ = false;
    bool handoff_ = false;             // opened straight from a closing palette
    int committed_row_ = -1;
    ImVec2 last_mouse_{-1, -1};
};

// Case-insensitive subsequence match with bonuses for word starts and runs.
bool fuzzy_match(const std::string& pattern, const std::string& text, int& score, std::vector<int>* positions);
