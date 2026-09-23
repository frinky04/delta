#pragma once
// The note editor: raw markdown with styles applied in place (symbols stay
// visible), plus a read-only reader view of the same lines. Everything is
// drawn by hand with ImDrawList into a centred column.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "markdown.h"
#include "smear.h"

struct TextPos {
    int line = 0, col = 0;  // col is a byte offset into the line
};
inline bool operator==(TextPos a, TextPos b) { return a.line == b.line && a.col == b.col; }
inline bool operator!=(TextPos a, TextPos b) { return !(a == b); }
inline bool operator<(TextPos a, TextPos b) { return a.line < b.line || (a.line == b.line && a.col < b.col); }

// A problem on a line: the message, and the byte range it's about
// (underlined; col0 < 0 for none).
struct Diagnostic {
    int line = 0, col0 = -1, col1 = -1;
    std::string msg;
};

struct EditorParams {
    ImFont* sans = nullptr;
    ImFont* mono = nullptr;
    ImFont* sans_bold = nullptr;  // SemiBold; null falls back to a double-drawn bold
    ImFont* mono_bold = nullptr;
    float size = 17;          // base pixel size
    float line_height = 1.6f;
    float column = 600;       // text column width in px
    ImVec2 min, max;          // area the editor owns
    float top_pad = 40;
    float dpi = 1;
    int tab_size = 4;
    bool line_numbers = true;
    bool blink = true;
    bool fade_syntax = true;  // dim markdown symbols away from the caret
    float blank_line = 0.6f;  // height of an empty line, in lines
    enum class Caret { Bar, Thick, Block } caret = Caret::Thick;
    bool caret_trail = false;  // the caret smears on its way to a new spot
    bool scroll_past_end = true;
    bool keyboard = true;     // take keyboard input this frame
    bool mouse = true;        // take mouse input this frame
    float alpha = 1;          // fade (note switch / mode switch / palette)
};

// Width of the bar carets for a body font of `size` (ImGui units). Block
// carets take the width of the character they cover instead.
inline float caret_width(EditorParams::Caret c, float size, float dpi) {
    if (c == EditorParams::Caret::Bar) return std::max(1.0f, std::round(1.2f * dpi));
    return std::max(std::round(3.0f * dpi), std::round(size * 0.22f));  // thick: about 0.3em
}

struct Heading { int line, level; std::string text; };

enum class Syntax { Markdown, Ini };

class Editor {
public:
    Editor();

    void set_text(const std::string& text);     // new document: resets undo, cursor, scroll
    void reload_text(const std::string& text);  // same document changed on disk: keeps cursor, undoable
    std::string text() const;
    uint64_t version() const { return version_; }

    void draw(const EditorParams& p);

    bool reader() const { return reader_; }
    void set_reader(bool r, bool animate = true);  // animate: morph between the views

    // Markdown for notes, Ini for delta.ini. Set before set_text().
    void set_syntax(Syntax s);
    Syntax syntax() const { return syntax_; }
    // Problems shown at the end of their (0-based) line. They fade in, and
    // out once they're gone from the list.
    void set_diagnostics(const std::vector<Diagnostic>& d);
    // Quieter notes shown the same way (config defaults); a problem on the
    // same line wins.
    void set_hints(std::map<int, std::string> h) { hints_ = std::move(h); }
    // Insert text as one undoable edit; returns where it ends.
    TextPos insert_text(TextPos at, const std::string& text);

    // Bindable edit commands.
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void select_all();
    void select_line();
    void move_lines(int dir);
    void duplicate_lines(int dir);
    void delete_lines();
    void toggle_task();
    void wrap_selection(const std::string& marker);
    void indent();
    void outdent();

    // Find and replace in this note. Smart case: case-sensitive only when the
    // query has a capital. The current match is selected; the rest are
    // softly highlighted.
    void find_begin();                            // remember where find started
    void find_set(const std::string& query);      // match from there as you type
    void find_step(int dir);                      // next (+1) / previous (-1), wrapping
    void find_end();                              // clear highlights, keep the selection
    int find_count() const { return (int)matches_.size(); }
    int find_index() const { return match_; }
    void replace_current(const std::string& with);
    int replace_all(const std::string& with);     // one undo step

    // Moving around.
    void goto_heading(int dir);                   // next / previous heading
    void expand_selection();                      // word, sentence, line, paragraph, section, note
    void shrink_selection();
    bool link_at_caret(LinkSpan& out) const;

    // Navigation.
    void goto_line(int line);                     // puts the line near the top
    void select_range(TextPos a, TextPos b);      // and centres it
    TextPos cursor() const { return cur_; }
    TextPos anchor() const { return anchor_; }
    void restore_view(TextPos cur, float scroll);
    float scroll() const { return scroll_target_; }
    bool has_selection() const { return cur_ != anchor_; }
    std::string selected_text() const;
    int selection_length() const;                 // in characters
    const std::vector<std::string>& lines() const { return lines_; }
    std::vector<Heading> headings() const;
    int word_count();

    // Where line i is drawn right now vs. where it's laid out (edit view).
    // They differ while space is opening or closing.
    float drawn_y(int i) const { return lay_[0][i].dy; }
    float laid_y(int i) const { return lay_[0][i].y; }

    // The span the change pulse is highlighting, if any.
    bool pulse_range(TextPos& a, TextPos& b) const {
        if (!pulse_.active || pulse_.spans.empty()) return false;
        a = pulse_.spans.front().first;
        b = pulse_.spans.front().second;
        return true;
    }
    int pulse_spans() const { return pulse_.active ? (int)pulse_.spans.size() : 0; }

    // When the caret next changes blink state (for the idle loop).
    double next_blink(double now) const;

    std::function<void(const LinkSpan&)> on_link;

private:
    struct Line {
        uint64_t hash = 0;
        float wrap = -1, base = -1, lh = -1, blank = -1;
        float hang = 0;                  // x where wrapped rows start
        float dy = 0, dh = 0;            // where it's drawn now (edit mode animates these)
        int tab = -1;
        bool in_code = false;
        LineInfo info;
        std::vector<uint16_t> style;
        std::string rtext;               // reader text
        std::vector<uint16_t> rstyle;
        std::vector<int> rmap;
        std::vector<int> rows;           // byte offset where each visual row starts
        float size = 0, row_h = 0, pad = 0, y = 0, h = 0;
    };
    struct Metrics {
        ImFontBaked* sans;
        ImFontBaked* mono;
        ImFontBaked* sans_b;  // may be null
        ImFontBaked* mono_b;
        float size, msize, tabw;
    };
    struct Snap {
        std::vector<std::string> lines;
        TextPos cur, anchor;
    };
    enum class Edit { None, Type, Delete, Other };

    // layout
    void layout(int mode);
    Metrics metrics(float size) const;
    bool bold_of(uint16_t style) const;
    float advance(const Metrics& m, unsigned cp, uint16_t style) const;
    const std::string& text_of(int mode, int i) const { return mode ? lay_[1][i].rtext : lines_[i]; }
    const std::vector<uint16_t>& style_of(int mode, int i) const { return mode ? lay_[1][i].rstyle : lay_[0][i].style; }
    float measure(const std::string& s, const std::vector<uint16_t>& st, int a, int b, const Metrics& m) const;
    int row_of(const Line& L, int col) const;
    static float row_x(const Line& L, int row) { return row > 0 ? L.hang : 0.0f; }
    int col_at_x(int mode, int line, int row, float x) const;
    ImVec2 pos_xy(TextPos p) const;              // doc coords of caret position (edit layout)
    bool hit(int mode, ImVec2 doc, int& line, int& col) const;
    int line_at_y(int mode, float y) const;

    // input
    void handle_keys();
    void handle_mouse(bool hovered, bool active, ImVec2 doc);
    void type_char(unsigned c);
    void newline();
    void backspace(bool word);
    void del(bool word);
    void tab();
    void move_to(TextPos p, bool select);
    void move_vertical(int rows, bool select);
    TextPos left_of(TextPos p, bool word) const;
    TextPos right_of(TextPos p, bool word) const;
    TextPos smart_home() const;
    TextPos row_end() const;
    void select_word_at(TextPos p);
    void toggle_task_line(int line);
    void pulse_changes(const std::vector<std::string>& before);

    // editing primitives
    void begin_edit(Edit kind);
    void end_edit(bool reveal = true);
    void erase(TextPos a, TextPos b);
    TextPos insert(TextPos p, const std::string& text);
    void delete_selection();
    void replace_selection(const std::string& text);
    bool in_code_before(int line) const;
    std::pair<int, int> selected_lines() const;
    void clamp(TextPos& p) const;

    // drawing
    void draw_line(ImDrawList* dl, int mode, int i, float x0, float y0);
    bool draw_morph(ImDrawList* dl, float s);  // edit <-> reader transition, s seconds in; false once settled
    void draw_line_body(ImDrawList* dl, int mode, const Line& L, const std::string& s,
                        const std::vector<uint16_t>& st, float y, int index, float x0, float shown_h);
    void sync_edits();               // pair lines before/after an edit
    void animate_layout(float dt);   // displayed heights chase real ones
    void resolve_pending();          // conserve space across the last structural edit
    void draw_row(ImDrawList* dl, int mode, const Line& L, const Metrics& m, const std::string& s,
                  const std::vector<uint16_t>& st, int a, int b, float x0, float ry);
    ImU32 fade(ImU32 c) const;

    std::vector<std::string> lines_{""};
    TextPos cur_, anchor_;
    float want_x_ = -1;
    bool reader_ = false;
    Syntax syntax_ = Syntax::Markdown;
    // A problem as drawn: it follows its line's text, so lines moving above
    // it don't restart its fade.
    struct Mark {
        Diagnostic d;
        std::string text;   // the line's text when it was set
        float a = 0;        // shown, 0..1
        bool live = true;   // false: fading out
    };
    std::vector<Mark> marks_;
    bool marks_snap_ = false;
    std::map<int, std::string> hints_;
    uint64_t version_ = 1;

    std::vector<Line> lay_[2];
    float total_h_[2] = {0, 0};
    EditorParams P_;
    float col_x_ = 0, origin_y_ = 0, view_h_ = 600;

    float scroll_ = 0, scroll_target_ = 0;
    bool snap_ = true;
    bool reveal_ = false;
    int reveal_mode_ = 0;  // 0 = just keep visible, 1 = near top, 2 = centre
    SmearRect caret_;
    bool caret_snap_ = true;
    double activity_ = 0, scroll_activity_ = -10;
    std::vector<float> yoff_;
    std::vector<float> mark_a_;      // per-line opacity of markdown syntax
    struct SelRect {
        int line, row;               // which visual row it covers
        float x1, x2, t1, t2;        // shown edges and where they're heading
        float a;                     // fades out once no longer selected
        bool live;
    };
    std::vector<SelRect> sel_rects_;

    // Soft highlight on what the last undo, redo, paste or replace changed.
    struct Pulse {
        bool active = false;
        std::vector<std::pair<TextPos, TextPos>> spans;
        double start = 0;
        uint64_t version = 0;       // any later edit clears it
    } pulse_;

    // Find state.
    bool find_active_ = false;
    std::string find_q_;
    TextPos find_origin_;
    std::vector<std::pair<TextPos, TextPos>> matches_;
    int match_ = -1;
    void find_recompute(TextPos from);           // matches for find_q_; current = first at/after `from`
    void select_match();

    // Expand-selection history, so shrink can walk back.
    std::vector<std::pair<TextPos, TextPos>> expand_stack_;
    std::pair<TextPos, TextPos> expand_last_;     // the selection expand made last

    TextPos paragraph_move(int dir) const;
    void renumber_lists(int l0, int l1);          // ordered lists touching these lines
    void draw_span(ImDrawList* dl, TextPos a, TextPos b, ImU32 col);

    // Space opening and closing. Each line's drawn height chases its real
    // height, and lines are stacked by drawn heights, so whatever opens or
    // closes pushes everything below it exactly. New lines: the space opens,
    // then the text fades in. Removed lines stay briefly as ghosts: the text
    // fades out, then the space closes. Taking turns, text never overlaps.
    // Lines are paired across an edit by comparing the text.
    struct Ghost {
        int before;                  // index of the line it sits above
        Line lay;
        std::string text;
        float h, full;
        bool show_text;              // false when its text was merged into a neighbour
        float a = 1;                 // fades out first; the space closes after
        bool fresh = false;          // from the edit resolve_pending() hasn't settled yet
        float dy = 0;
    };
    std::vector<std::string> prev_lines_;
    uint64_t diff_version_ = 0;
    std::vector<float> dh_;          // drawn height per line (edit layout)
    std::vector<Ghost> ghosts_;
    bool snap_heights_ = true;
    struct Morph {
        bool active = false;
        bool to_reader = true;
        double start = 0;
        float scroll_from = 0;       // scroll of the view being left
    } morph_;
    struct Pending {
        bool active = false;
        int begin = 0;               // first new line of the edited region
        std::vector<char> is_new;    // per line in the region: inserted (vs carried on)
        float old_space = 0;         // drawn space the region had before the edit
    } pending_;
    float geom_[5] = {};             // column, size, line height, blank, tab: a change snaps
    TextPos sel_cur_, sel_anchor_;
    uint64_t sel_version_ = 0;
    float line_mark_a_ = 1;          // the line being drawn
    bool exact_x_ = false;           // morph: text x is pre-snapped, don't round it again
    int sync_line_ = -1;
    float sync_frac_ = 0;

    std::vector<Snap> undo_, redo_;
    Edit last_edit_ = Edit::None;
    double last_edit_time_ = 0;
    TextPos last_edit_pos_;
    std::string line_clip_;

    int drag_mode_ = 0;  // 1 char, 2 word, 3 line
    TextPos drag_a_, drag_b_;

    int words_ = 0;
    uint64_t words_version_ = 0;
};
