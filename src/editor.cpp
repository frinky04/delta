#include "editor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ini_syntax.h"
#include "theme.h"
#include "util.h"

namespace {

uint64_t fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// 0 = whitespace, 1 = word, 2 = punctuation
int char_class(unsigned char c) {
    if (c == ' ' || c == '\t') return 0;
    if (std::isalnum(c) || c == '_' || c >= 0x80) return 1;
    return 2;
}

bool is_mono(Style f) { return f & (S_Code | S_CodeBlock | S_Tag); }

}  // namespace

Editor::Editor() = default;

// ---------------------------------------------------------------------------
// Document

void Editor::set_text(const std::string& text) {
    lines_ = split_lines(text);
    cur_ = anchor_ = {};
    undo_.clear();
    redo_.clear();
    last_edit_ = Edit::None;
    want_x_ = -1;
    version_++;
    lay_[0].clear();
    lay_[1].clear();
    yoff_.assign(lines_.size(), 0);
    mark_a_.clear();
    sel_rects_.clear();
    find_end();
    expand_stack_.clear();
    pulse_.active = false;
    // A different document appears as it is: nothing opens or collapses.
    ghosts_.clear();
    snap_heights_ = true;
    prev_lines_ = lines_;
    diff_version_ = version_;
    scroll_ = scroll_target_ = 0;
    snap_ = caret_snap_ = true;
    sync_line_ = -1;
    marks_.clear();
    marks_snap_ = true;  // problems set before the first draw arrive with the page
    comp_.open = false;
    comp_dismissed_line_ = -1;
}

void Editor::set_diagnostics(const std::vector<Diagnostic>& ds) {
    for (auto& m : marks_) m.live = false;
    for (auto& d : ds) {
        if (d.line < 0 || d.line >= (int)lines_.size()) continue;
        const std::string& text = lines_[d.line];
        Mark* same = nullptr;
        for (auto& m : marks_)
            if (!m.live && m.d.kind == d.kind && m.d.col0 == d.col0 && m.d.msg == d.msg && m.text == text) { same = &m; break; }
        if (same) {
            same->d = d;
            same->live = true;
        } else {
            marks_.push_back({d, text, marks_snap_ ? 1.0f : 0.0f, true});
        }
    }
}

void Editor::reload_text(const std::string& text) {
    begin_edit(Edit::Other);
    lines_ = split_lines(text);
    clamp(cur_);
    clamp(anchor_);
    end_edit(false);
}

std::string Editor::text() const { return join_lines(lines_); }

void Editor::clamp(TextPos& p) const {
    p.line = std::clamp(p.line, 0, (int)lines_.size() - 1);
    p.col = std::clamp(p.col, 0, (int)lines_[p.line].size());
    while (p.col > 0 && utf8::is_cont((unsigned char)lines_[p.line][p.col])) p.col--;
}

std::string Editor::selected_text() const {
    if (!has_selection()) return {};
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    if (a.line == b.line) return lines_[a.line].substr(a.col, b.col - a.col);
    std::string out = lines_[a.line].substr(a.col);
    for (int l = a.line + 1; l < b.line; l++) out += "\n" + lines_[l];
    out += "\n" + lines_[b.line].substr(0, b.col);
    return out;
}

int Editor::selection_length() const {
    // Counted in place (the status bar asks every frame): characters, plus
    // one for each line break.
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    int n = b.line - a.line;
    for (int l = a.line; l <= b.line; l++) {
        const std::string& s = lines_[l];
        int from = l == a.line ? a.col : 0, to = l == b.line ? b.col : (int)s.size();
        for (int i = from; i < to; i++)
            if (!utf8::is_cont((unsigned char)s[i])) n++;
    }
    return n;
}

std::vector<Heading> Editor::headings() const {
    std::vector<Heading> out;
    if (syntax_ == Syntax::Ini) {
        std::string name;
        for (int i = 0; i < (int)lines_.size(); i++)
            if (ini_section(lines_[i], name)) out.push_back({i, 1, name});
        return out;
    }
    bool in_code = false;
    LineInfo info;
    std::vector<Style> st;
    for (int i = 0; i < (int)lines_.size(); i++) {
        md_analyze(lines_[i], in_code, info, st);
        if (info.kind == LineKind::Fence) in_code = !in_code;
        if (info.kind == LineKind::Heading) out.push_back({i, info.level, trim(lines_[i].substr(info.content_start))});
    }
    return out;
}

int Editor::word_count() {
    if (words_version_ == version_) return words_;
    int n = 0;
    for (auto& l : lines_) {
        if (md_image_line(l)) continue;  // an image isn't words
        bool in_word = false, has_alnum = false;
        for (size_t i = 0; i <= l.size(); i++) {
            unsigned char c = i < l.size() ? (unsigned char)l[i] : ' ';
            if (c == ' ' || c == '\t') {
                if (in_word && has_alnum) n++;
                in_word = has_alnum = false;
            } else {
                in_word = true;
                if (md_word_char(c)) has_alnum = true;
            }
        }
    }
    words_ = n;
    words_version_ = version_;
    return n;
}

bool Editor::in_code_before(int line) const {
    bool in_code = false;
    for (int i = 0; i < line && i < (int)lines_.size(); i++)
        if (md_is_fence(lines_[i])) in_code = !in_code;
    return in_code;
}

std::pair<int, int> Editor::selected_lines() const {
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    if (b.line > a.line && b.col == 0) b.line--;
    return {a.line, b.line};
}

// ---------------------------------------------------------------------------
// Edit primitives

Editor::Snap Editor::snapshot() const {
    Snap s{lines_, cur_, anchor_};
    s.bytes = lines_.size() * sizeof(std::string);
    for (auto& l : lines_) s.bytes += l.size();
    return s;
}

void Editor::begin_edit(Edit kind) {
    double now = ImGui::GetTime();
    bool merge = kind != Edit::Other && kind == last_edit_ && now - last_edit_time_ < 1.2 &&
                 cur_ == last_edit_pos_ && !has_selection();
    if (!merge) {
        // Every step is a whole copy of the note, so a big note keeps fewer
        // of them: at most 500 steps, and at most kUndoBytes in all.
        const size_t kUndoBytes = (size_t)128 << 20;
        undo_.push_back(snapshot());
        size_t total = 0;
        for (auto& s : undo_) total += s.bytes;
        size_t drop = 0;
        while (undo_.size() - drop > 1 && (undo_.size() - drop > 500 || total > kUndoBytes)) total -= undo_[drop++].bytes;
        undo_.erase(undo_.begin(), undo_.begin() + drop);
    }
    redo_.clear();
    last_edit_ = kind;
    last_edit_time_ = now;
}

void Editor::end_edit(bool reveal) {
    last_edit_pos_ = cur_;
    version_++;
    if (reveal) { reveal_ = true; reveal_mode_ = 0; }
    activity_ = ImGui::GetTime();
    want_x_ = -1;
    if (yoff_.size() != lines_.size()) yoff_.assign(lines_.size(), 0);
}

void Editor::erase(TextPos a, TextPos b) {
    if (b < a) std::swap(a, b);
    if (a.line == b.line) {
        lines_[a.line].erase(a.col, b.col - a.col);
    } else {
        lines_[a.line] = lines_[a.line].substr(0, a.col) + lines_[b.line].substr(b.col);
        lines_.erase(lines_.begin() + a.line + 1, lines_.begin() + b.line + 1);
    }
}

TextPos Editor::insert(TextPos p, const std::string& text) {
    std::vector<std::string> parts = split_lines(text);
    std::string& line = lines_[p.line];
    if (parts.size() == 1) {
        line.insert(p.col, parts[0]);
        return {p.line, p.col + (int)parts[0].size()};
    }
    std::string tail = line.substr(p.col);
    line = line.substr(0, p.col) + parts[0];
    lines_.insert(lines_.begin() + p.line + 1, parts.begin() + 1, parts.end());
    int last = p.line + (int)parts.size() - 1;
    int col = (int)lines_[last].size();
    lines_[last] += tail;
    return {last, col};
}

void Editor::delete_selection() {
    if (!has_selection()) return;
    TextPos a = std::min(cur_, anchor_);
    erase(cur_, anchor_);
    cur_ = anchor_ = a;
}

void Editor::replace_selection(const std::string& text) {
    delete_selection();
    cur_ = anchor_ = insert(cur_, text);
}

// ---------------------------------------------------------------------------
// Commands

void Editor::undo() { step_history(undo_, redo_); }
void Editor::redo() { step_history(redo_, undo_); }

// Back to the last snapshot in `from`, noting where it was in `to`.
void Editor::step_history(std::vector<Snap>& from, std::vector<Snap>& to) {
    if (from.empty()) return;
    to.push_back(snapshot());
    Snap s = std::move(from.back());
    from.pop_back();
    std::vector<std::string> before = std::move(lines_);
    lines_ = std::move(s.lines);
    cur_ = s.cur;
    anchor_ = s.anchor;
    last_edit_ = Edit::None;
    end_edit();
    pulse_changes(before);
}

void Editor::copy() {
    if (has_selection()) {
        ImGui::SetClipboardText(selected_text().c_str());
        line_clip_.clear();
    } else {
        line_clip_ = lines_[cur_.line] + "\n";
        ImGui::SetClipboardText(line_clip_.c_str());
    }
}

void Editor::cut() {
    copy();
    begin_edit(Edit::Other);
    if (has_selection()) {
        delete_selection();
    } else if (lines_.size() == 1) {
        lines_[0].clear();
        cur_ = anchor_ = {};
    } else {
        int l = cur_.line;
        lines_.erase(lines_.begin() + l);
        cur_ = {std::min(l, (int)lines_.size() - 1), cur_.col};
        clamp(cur_);
        anchor_ = cur_;
    }
    renumber_lists(cur_.line - 1, cur_.line);
    end_edit();
}

void Editor::paste() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) return;
    std::string text = clip;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::vector<std::string> before = lines_;
    TextPos start = std::min(cur_, anchor_);
    begin_edit(Edit::Other);
    // A URL pasted over selected text (on one line) makes a link of it.
    bool url = (text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0) &&
               text.find_first_of(" \t\n") == std::string::npos;
    if (url && has_selection() && cur_.line == anchor_.line && !is_img(cur_.line) && syntax_ == Syntax::Markdown) {
        replace_selection("[" + selected_text() + "](" + text + ")");
    } else if (!has_selection() && !line_clip_.empty() && text == line_clip_) {
        // A whole-line copy pastes as a line above, like an IDE.
        int col = cur_.col;
        TextPos end = insert({cur_.line, 0}, text);
        cur_ = anchor_ = {end.line, col};
    } else {
        step_off_image();
        replace_selection(text);
    }
    renumber_lists(start.line, cur_.line);
    end_edit();
    pulse_changes(before);
}

TextPos Editor::insert_text(TextPos at, const std::string& text) {
    clamp(at);
    std::vector<std::string> before = lines_;
    begin_edit(Edit::Other);
    TextPos end = insert(at, text);
    cur_ = anchor_ = end;
    end_edit();
    pulse_changes(before);
    return end;
}

void Editor::insert_lines(const std::vector<std::string>& block) {
    if (block.empty()) return;
    std::vector<std::string> before = lines_;
    begin_edit(Edit::Other);
    unpick();  // they go after a picked image, not over it
    delete_selection();
    step_off_image();
    int l = cur_.line;
    const std::string s = lines_[l];
    int at;  // where the block's first line goes
    if (trim(s).empty()) {
        lines_.erase(lines_.begin() + l);  // an empty line takes them
        at = l;
    } else if (cur_.col == 0) {
        at = l;
    } else if (cur_.col >= (int)s.size()) {
        at = l + 1;
    } else {
        lines_[l] = s.substr(0, cur_.col);
        lines_.insert(lines_.begin() + l + 1, s.substr(cur_.col));
        at = l + 1;
    }
    lines_.insert(lines_.begin() + at, block.begin(), block.end());
    int last = at + (int)block.size() - 1;
    cur_ = anchor_ = {last, (int)lines_[last].size()};
    end_edit();
    pulse_changes(before);
}

bool Editor::replace_line_everywhere(const std::string& from, const std::string& to) {
    bool found = false;
    auto swap_in = [&](std::vector<std::string>& v) {
        for (auto& l : v)
            if (l == from) { l = to; found = true; }
    };
    swap_in(lines_);
    for (auto& s : undo_) swap_in(s.lines);
    for (auto& s : redo_) swap_in(s.lines);
    if (found) {
        for (auto* p : {&cur_, &anchor_}) clamp(*p);
        version_++;
    }
    return found;
}

TextPos Editor::position_at(ImVec2 screen) const {
    int line = 0, col = 0;
    if (!hit(0, ImVec2(screen.x - col_x_, screen.y - origin_y_), line, col)) return cur_;
    return {line, col};
}

// Highlight what an undo, redo or paste changed: the span of text that
// differs between `before` and now, down to the character. Nothing to show
// for a pure deletion (the removed lines' fade already says what happened).
void Editor::pulse_changes(const std::vector<std::string>& before) {
    pulse_.active = false;
    std::string o = join_lines(before), n = join_lines(lines_);
    size_t pre = 0, lim = std::min(o.size(), n.size());
    while (pre < lim && o[pre] == n[pre]) pre++;
    size_t suf = 0;
    while (suf < lim - pre && o[o.size() - 1 - suf] == n[n.size() - 1 - suf]) suf++;
    // Snap to whole characters.
    while (pre > 0 && utf8::is_cont((unsigned char)n[pre])) pre--;
    size_t end = n.size() - suf;
    while (end < n.size() && utf8::is_cont((unsigned char)n[end])) end++;
    if (end <= pre) return;

    auto pos_of = [&](size_t off) {
        TextPos p;
        for (size_t i = 0; i < off; i++) {
            if (n[i] == '\n') { p.line++; p.col = 0; }
            else p.col++;
        }
        return p;
    };
    pulse({{pos_of(pre), pos_of(end)}});
}

// Light these spans up (none: no pulse), until the next edit.
void Editor::pulse(std::vector<std::pair<TextPos, TextPos>> spans) {
    pulse_.spans = std::move(spans);
    pulse_.start = anim::clock;
    pulse_.version = version_;
    pulse_.active = !pulse_.spans.empty();
}

// ---------------------------------------------------------------------------
// Find and replace

void Editor::find_begin() {
    find_active_ = true;
    find_origin_ = std::min(cur_, anchor_);
}

void Editor::find_recompute(TextPos from) {
    matches_.clear();
    match_ = -1;
    matches_version_ = version_;
    if (find_q_.empty()) return;
    // Smart case: a capital in the query makes it case-sensitive.
    bool exact = std::any_of(find_q_.begin(), find_q_.end(), [](char c) { return std::isupper((unsigned char)c); });
    std::string q = exact ? find_q_ : lower(find_q_);
    for (int l = 0; l < (int)lines_.size() && matches_.size() < 10000; l++) {
        if (is_img(l)) continue;  // its text is hidden: nothing to find there
        std::string s = exact ? lines_[l] : lower(lines_[l]);
        for (size_t at = s.find(q); at != std::string::npos; at = s.find(q, at + q.size()))
            matches_.push_back({{l, (int)at}, {l, (int)(at + q.size())}});
    }
    if (matches_.empty()) return;
    match_ = 0;
    for (int k = 0; k < (int)matches_.size(); k++)
        if (!(matches_[k].first < from)) { match_ = k; break; }
}

// An undo, a reload from disk or an edit while find is open leaves the
// matches pointing at text that is gone; find them again near the current one.
void Editor::find_refresh() {
    if (!find_active_ || matches_version_ == version_) return;
    bool have = match_ >= 0 && match_ < (int)matches_.size();
    find_recompute(have ? matches_[match_].first : find_origin_);
}

void Editor::select_match() {
    if (match_ < 0 || match_ >= (int)matches_.size()) return;
    anchor_ = matches_[match_].first;
    cur_ = matches_[match_].second;
    reveal_ = true;
    reveal_mode_ = 3;
    want_x_ = -1;
    activity_ = ImGui::GetTime();
}

void Editor::find_set(const std::string& query) {
    find_active_ = true;
    find_q_ = query;
    find_recompute(find_origin_);
    if (match_ >= 0) select_match();
    else cur_ = anchor_ = find_origin_;  // nothing found: back to where you started
}

void Editor::find_step(int dir) {
    find_refresh();
    if (matches_.empty()) return;
    int n = (int)matches_.size();
    match_ = ((match_ < 0 ? 0 : match_) + dir + n) % n;
    select_match();
    find_origin_ = matches_[match_].first;  // editing the query continues from here
}

void Editor::find_end() {
    find_active_ = false;
    matches_.clear();
    match_ = -1;
}

void Editor::replace_range(TextPos a, TextPos b, const std::string& with) {
    clamp(a);
    clamp(b);
    if (b < a) std::swap(a, b);
    begin_edit(Edit::Other);
    erase(a, b);
    TextPos e = insert(a, with);
    cur_ = anchor_ = e;
    end_edit();
    if (e != a) pulse({{a, e}});
    else pulse_.active = false;
}

void Editor::replace_current(const std::string& with) {
    find_refresh();
    if (match_ < 0 || match_ >= (int)matches_.size()) return;
    replace_range(matches_[match_].first, matches_[match_].second, with);
    // On to the next match after what was just written.
    find_origin_ = cur_;
    find_recompute(cur_);
    if (match_ >= 0) select_match();
}

int Editor::replace_all(const std::string& with) {
    find_refresh();
    if (matches_.empty()) return 0;
    int count = (int)matches_.size();
    // Where each replacement will land: matches are single-line, so only
    // earlier matches on the same line shift it.
    std::vector<std::pair<TextPos, TextPos>> spans;
    int line = -1, shift = 0;
    for (auto& [a, b] : matches_) {
        if (a.line != line) { line = a.line; shift = 0; }
        TextPos s{a.line, a.col + shift};
        spans.push_back({s, {a.line, s.col + (int)with.size()}});
        shift += (int)with.size() - (b.col - a.col);
    }
    TextPos current = spans[std::clamp(match_, 0, count - 1)].second;
    begin_edit(Edit::Other);  // one undo step for the lot
    for (int k = count - 1; k >= 0; k--) {
        erase(matches_[k].first, matches_[k].second);
        insert(matches_[k].first, with);
    }
    cur_ = anchor_ = current;
    end_edit();
    if (with.empty()) spans.clear();
    pulse(std::move(spans));
    find_origin_ = current;
    find_recompute(current);
    return count;
}

// ---------------------------------------------------------------------------
// Moving around

void Editor::goto_heading(int dir) {
    auto hs = headings();
    int target = -1;
    if (dir > 0) {
        for (auto& h : hs) if (h.line > cur_.line) { target = h.line; break; }
    } else {
        for (auto& h : hs) if (h.line < cur_.line) target = h.line;
    }
    if (target < 0) return;
    int col = 0;
    if (syntax_ == Syntax::Markdown) {
        LineInfo info;
        std::vector<Style> st;
        md_analyze(lines_[target], in_code_before(target), info, st);
        col = info.content_start;
    }
    move_to({target, col}, false);
    reveal_mode_ = 3;
    want_x_ = -1;
}

// Start of the next paragraph (dir > 0), or the start of this one / the
// previous one (dir < 0). Paragraphs are separated by blank lines.
TextPos Editor::paragraph_move(int dir) const {
    const int n = (int)lines_.size();
    auto blank = [&](int l) { return trim(lines_[l]).empty(); };
    int l = cur_.line;
    if (dir > 0) {
        while (l < n && !blank(l)) l++;
        while (l < n && blank(l)) l++;
        if (l >= n) return {n - 1, (int)lines_[n - 1].size()};
        return {l, 0};
    }
    if (cur_.col > 0 && !blank(l)) {
        while (l > 0 && !blank(l - 1)) l--;
        return {l, 0};
    }
    l--;
    while (l >= 0 && blank(l)) l--;
    if (l < 0) return {0, 0};
    while (l > 0 && !blank(l - 1)) l--;
    return {l, 0};
}

// Grow the selection one step: word, sentence, line (text), whole line,
// paragraph, the section under its heading, the whole note.
void Editor::expand_selection() {
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    if (expand_stack_.empty() || !(expand_last_.first == a && expand_last_.second == b)) expand_stack_.clear();
    const int n = (int)lines_.size();
    auto contains = [&](TextPos ca, TextPos cb) { return !(a < ca) && !(cb < b) && (ca < a || b < cb); };
    auto blank = [&](int l) { return trim(lines_[l]).empty(); };
    std::vector<std::pair<TextPos, TextPos>> cands;

    const std::string& s = lines_[a.line];
    LineInfo info;
    std::vector<Style> st;
    if (syntax_ == Syntax::Markdown) md_analyze(s, in_code_before(a.line), info, st);
    int cs = std::min(info.content_start, (int)s.size()), ce = (int)s.size();
    if (a.line == b.line) {
        // Word.
        int wa = a.col, wb = b.col;
        auto word = [&](int i) { return i >= 0 && i < (int)s.size() && char_class(s[i]) == 1; };
        while (wa > 0 && word(wa - 1)) wa--;
        while (wb < (int)s.size() && word(wb)) wb++;
        if (wb > wa) cands.push_back({{a.line, wa}, {a.line, wb}});
        // Sentence: between sentence ends (. ! ? then a space) within the line.
        int sa = cs;
        for (int i = std::min(a.col, ce) - 1; i >= cs; i--)
            if (std::strchr(".!?", s[i]) && i + 1 < ce && s[i + 1] == ' ') { sa = i + 1; break; }
        while (sa < ce && s[sa] == ' ') sa++;
        int sb = ce;
        for (int i = std::max(b.col, sa); i < ce; i++)
            if (std::strchr(".!?", s[i]) && (i + 1 == ce || s[i + 1] == ' ')) { sb = i + 1; break; }
        cands.push_back({{a.line, sa}, {a.line, sb}});
        cands.push_back({{a.line, cs}, {a.line, ce}});   // the line's text, without its marker
        cands.push_back({{a.line, 0}, {a.line, ce}});    // the whole line
    }
    // Paragraph.
    {
        int p0 = a.line, p1 = b.line;
        while (p0 > 0 && !blank(p0 - 1)) p0--;
        while (p1 + 1 < n && !blank(p1 + 1)) p1++;
        cands.push_back({{p0, 0}, {p1, (int)lines_[p1].size()}});
    }
    // Section: from the heading above to the next heading of the same or a
    // higher level.
    {
        auto hs = headings();
        for (int k = (int)hs.size() - 1; k >= 0; k--) {
            if (hs[k].line > a.line) continue;
            int end = n - 1;
            for (size_t j = k + 1; j < hs.size(); j++)
                if (hs[j].level <= hs[k].level) { end = hs[j].line - 1; break; }
            while (end > hs[k].line && blank(end)) end--;
            cands.push_back({{hs[k].line, 0}, {end, (int)lines_[end].size()}});
        }
    }
    cands.push_back({{0, 0}, {n - 1, (int)lines_[n - 1].size()}});

    for (auto& [ca, cb] : cands) {
        if (!contains(ca, cb)) continue;
        expand_stack_.push_back({anchor_, cur_});
        anchor_ = ca;
        cur_ = cb;
        expand_last_ = {ca, cb};
        activity_ = ImGui::GetTime();
        return;
    }
}

void Editor::shrink_selection() {
    TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
    if (expand_stack_.empty() || !(expand_last_.first == a && expand_last_.second == b)) {
        expand_stack_.clear();
        return;
    }
    auto [an, cu] = expand_stack_.back();
    expand_stack_.pop_back();
    anchor_ = an;
    cur_ = cu;
    expand_last_ = {std::min(an, cu), std::max(an, cu)};
    activity_ = ImGui::GetTime();
}

bool Editor::link_at_caret(LinkSpan& out) const {
    if (syntax_ != Syntax::Markdown) return false;
    std::vector<LinkSpan> links;
    LineInfo info;
    std::vector<Style> st;
    md_analyze(lines_[cur_.line], in_code_before(cur_.line), info, st, &links);
    for (auto& l : links)
        if (cur_.col >= l.begin && cur_.col <= l.end) { out = l; return true; }
    return false;
}

// Keep "1. 2. 3." lists numbered after items are added, removed or moved.
// Renumbers the ordered lists touching lines l0..l1 (plus a line either
// side), from each list's first number. Items of a nested list are a list
// of their own. Runs inside an edit, so it's part of the same undo step.
void Editor::renumber_lists(int l0, int l1) {
    if (syntax_ != Syntax::Markdown) return;
    const int n = (int)lines_.size();
    // Only the lists near l0..l1 are looked at, so lines are analyzed as the
    // walk reaches them; which lines are inside code comes from the fences.
    std::vector<bool> code = md_code_lines(lines_);
    std::vector<LineInfo> infos(n);
    std::vector<char> have(n, 0);
    std::vector<Style> st;
    auto info = [&](int k) -> const LineInfo& {
        if (!have[k]) {
            md_analyze(lines_[k], code[k], infos[k], st);
            have[k] = 1;
        }
        return infos[k];
    };
    auto item = [&](int k, int indent) {
        return info(k).kind == LineKind::Para && info(k).ordered && info(k).marker_begin == indent;
    };
    auto nested = [&](int k, int indent) {  // deeper lines that belong to an item
        return info(k).kind == LineKind::Para && info(k).marker_begin > indent;
    };
    std::vector<char> done(n, 0);
    for (int l = std::max(0, l0 - 1); l <= std::min(n - 1, l1 + 1); l++) {
        if (done[l] || info(l).kind != LineKind::Para || !info(l).ordered) continue;
        int indent = info(l).marker_begin;
        int s = l, e = l;
        while (s > 0 && (item(s - 1, indent) || nested(s - 1, indent))) s--;
        while (e + 1 < n && (item(e + 1, indent) || nested(e + 1, indent))) e++;
        long number = -1;
        for (int k = s; k <= e; k++) {
            if (!item(k, indent)) continue;
            done[k] = 1;
            std::string& text = lines_[k];
            int d = indent;
            while (d < (int)text.size() && std::isdigit((unsigned char)text[d])) d++;
            if (number < 0) { number = std::strtol(text.c_str() + indent, nullptr, 10); continue; }
            number++;
            std::string want = std::to_string(number);
            int have = d - indent;
            if (text.compare(indent, have, want) == 0) continue;
            text.replace(indent, have, want);
            int delta = (int)want.size() - have;
            for (TextPos* p : {&cur_, &anchor_})
                if (p->line == k && p->col > indent) p->col = std::max(indent, p->col + delta);
        }
    }
}

void Editor::select_all() {
    anchor_ = {};
    cur_ = {(int)lines_.size() - 1, (int)lines_.back().size()};
    activity_ = ImGui::GetTime();
}

void Editor::select_line() {
    auto [a, b] = selected_lines();
    if (has_selection()) b++;  // extend
    anchor_ = {a, 0};
    if (b + 1 < (int)lines_.size()) cur_ = {b + 1, 0};
    else cur_ = {(int)lines_.size() - 1, (int)lines_.back().size()};
    reveal_ = true;
    activity_ = ImGui::GetTime();
}

void Editor::move_lines(int dir) {
    int tile = 0;
    if (int line = picked(&tile); line >= 0 && move_picked(line, tile, dir)) return;
    auto [a, b] = selected_lines();
    int n = (int)lines_.size();
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= n)) return;

    auto& V = lay_[0];
    bool have = (int)V.size() == n && (int)yoff_.size() == n;
    float block_h = 0, other_h = 0;
    if (have) {
        // Images are noted where they're drawn before the offsets below are
        // rotated (sync_edits would note them after, against the wrong
        // lines): they slide with their lines and need no morph of their own.
        shoot_tiles();
        for (int l = a; l <= b; l++) block_h += V[l].h;
        other_h = V[dir < 0 ? a - 1 : b + 1].h;
    }

    begin_edit(Edit::Other);
    if (dir < 0) {
        std::rotate(lines_.begin() + a - 1, lines_.begin() + a, lines_.begin() + b + 1);
        if (have) std::rotate(yoff_.begin() + a - 1, yoff_.begin() + a, yoff_.begin() + b + 1);
    } else {
        std::rotate(lines_.begin() + a, lines_.begin() + b + 1, lines_.begin() + b + 2);
        if (have) std::rotate(yoff_.begin() + a, yoff_.begin() + b + 1, yoff_.begin() + b + 2);
    }
    cur_.line += dir;
    anchor_.line += dir;
    renumber_lists(std::min(a, a + dir), std::max(b, b + dir));
    end_edit();

    // Lines start where they were and slide into their new slots.
    if (have) {
        for (int l = a + dir; l <= b + dir; l++) yoff_[l] += dir < 0 ? other_h : -other_h;
        yoff_[dir < 0 ? b : a] += dir < 0 ? -block_h : block_h;
    }
}

void Editor::duplicate_lines(int dir) {
    auto [a, b] = selected_lines();
    begin_edit(Edit::Other);
    std::vector<std::string> block(lines_.begin() + a, lines_.begin() + b + 1);
    lines_.insert(lines_.begin() + b + 1, block.begin(), block.end());
    if (dir > 0) {
        int count = b - a + 1;
        cur_.line += count;
        anchor_.line += count;
    }
    renumber_lists(a, b + (b - a + 1));
    end_edit();
}

void Editor::delete_lines() {
    auto [a, b] = selected_lines();
    begin_edit(Edit::Other);
    lines_.erase(lines_.begin() + a, lines_.begin() + b + 1);
    if (lines_.empty()) lines_.emplace_back();
    cur_ = {std::min(a, (int)lines_.size() - 1), cur_.col};
    clamp(cur_);
    anchor_ = cur_;
    renumber_lists(a - 1, a);
    end_edit();
}

void Editor::toggle_task_line(int l) {
    LineInfo info;
    std::vector<Style> st;
    md_analyze(lines_[l], in_code_before(l), info, st);
    if (info.kind == LineKind::Fence || info.kind == LineKind::CodeBlock || info.kind == LineKind::Heading) return;
    std::string& s = lines_[l];
    auto shift = [&](TextPos& p, int at, int delta) {
        if (p.line == l && p.col >= at) p.col += delta;
    };
    if (info.task) {
        for (int i = 0; i + 1 < (int)s.size(); i++)
            if (st[i] & S_TaskBox) { s[i + 1] = info.done ? ' ' : 'x'; break; }
    } else if (info.list) {
        s.insert(info.content_start, "[ ] ");
        shift(cur_, info.content_start, 4);
        shift(anchor_, info.content_start, 4);
    } else {
        int at = info.marker_begin;
        s.insert(at, "- [ ] ");
        shift(cur_, at, 6);
        shift(anchor_, at, 6);
    }
}

void Editor::toggle_task() {
    auto [a, b] = selected_lines();
    begin_edit(Edit::Other);
    for (int l = a; l <= b; l++) toggle_task_line(l);
    end_edit();
}

void Editor::wrap_selection(const std::string& m) {
    if (is_img(cur_.line)) return;  // marks would turn the image back into text
    int ml = (int)m.size();
    begin_edit(Edit::Other);
    if (!has_selection()) {
        insert(cur_, m + m);
        cur_.col += ml;
        anchor_ = cur_;
    } else if (cur_.line == anchor_.line) {
        bool fwd = anchor_ < cur_;
        TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
        const std::string& s = lines_[a.line];
        bool wrapped = a.col >= ml && b.col + ml <= (int)s.size() && s.compare(a.col - ml, ml, m) == 0 &&
                       s.compare(b.col, ml, m) == 0;
        if (wrapped) {
            lines_[a.line].erase(b.col, ml);
            lines_[a.line].erase(a.col - ml, ml);
            a.col -= ml;
            b.col -= ml;
        } else {
            lines_[a.line].insert(b.col, m);
            lines_[a.line].insert(a.col, m);
            a.col += ml;
            b.col += ml;
        }
        anchor_ = fwd ? a : b;
        cur_ = fwd ? b : a;
    } else {
        TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
        insert(b, m);
        insert(a, m);
        if (cur_.line == a.line) cur_.col += ml;
        if (anchor_.line == a.line) anchor_.col += ml;
    }
    end_edit();
}

void Editor::indent() {
    auto [a, b] = selected_lines();
    std::string pad(P_.tab_size, ' ');
    begin_edit(Edit::Other);
    for (int l = a; l <= b; l++) {
        if (lines_[l].empty() && a != b) continue;
        lines_[l].insert(0, pad);
        if (cur_.line == l) cur_.col += P_.tab_size;
        if (anchor_.line == l) anchor_.col += P_.tab_size;
    }
    renumber_lists(a, b);
    end_edit();
}

void Editor::outdent() {
    auto [a, b] = selected_lines();
    begin_edit(Edit::Other);
    for (int l = a; l <= b; l++) {
        std::string& s = lines_[l];
        int k = 0;
        if (!s.empty() && s[0] == '\t') k = 1;
        else while (k < P_.tab_size && k < (int)s.size() && s[k] == ' ') k++;
        s.erase(0, k);
        if (cur_.line == l) cur_.col = std::max(0, cur_.col - k);
        if (anchor_.line == l) anchor_.col = std::max(0, anchor_.col - k);
    }
    renumber_lists(a, b);
    end_edit();
}

void Editor::goto_line(int line) {
    cur_ = anchor_ = {std::clamp(line, 0, (int)lines_.size() - 1), 0};
    reveal_ = true;
    reveal_mode_ = 1;
    activity_ = ImGui::GetTime();
}

void Editor::select_range(TextPos a, TextPos b) {
    clamp(a);
    clamp(b);
    anchor_ = a;
    cur_ = b;
    reveal_ = true;
    reveal_mode_ = 2;
    activity_ = ImGui::GetTime();
}

void Editor::jump_to(TextPos p) {
    clamp(p);
    cur_ = anchor_ = p;
    reveal_ = true;
    reveal_mode_ = 3;
    want_x_ = -1;
    activity_ = ImGui::GetTime();
}

void Editor::return_to(TextPos cur, TextPos anchor, float scroll) {
    clamp(cur);
    clamp(anchor);
    cur_ = cur;
    anchor_ = anchor;
    scroll_target_ = scroll;
    reveal_ = false;
    want_x_ = -1;
    activity_ = ImGui::GetTime();
}

void Editor::restore_view(TextPos cur, float scroll) {
    clamp(cur);
    cur_ = anchor_ = cur;
    scroll_ = scroll_target_ = scroll;
    caret_snap_ = true;
}

void Editor::fonts_changed() {
    lay_[0].clear();  // widths are cached per line
    lay_[1].clear();
    snap_heights_ = true;
}

void Editor::set_syntax(Syntax s) {
    if (s == syntax_) return;
    syntax_ = s;
    lay_[0].clear();  // styles are cached per line
    lay_[1].clear();
    snap_heights_ = true;
    marks_.clear();
    hints_.clear();
}

void Editor::set_reader(bool r, bool animate) {
    if (r == reader_) return;
    int mode = reader_ ? 1 : 0;
    auto& V = lay_[mode];
    sync_line_ = -1;
    float doc_top = scroll_ - P_.top_pad;
    if (V.size() == lines_.size() && doc_top > 0) {
        int i = line_at_y(mode, doc_top);
        sync_line_ = i;
        sync_frac_ = V[i].h > 0 ? std::clamp((doc_top - V[i].y) / V[i].h, 0.0f, 1.0f) : 0;
    }
    // The page morphs between the two views: words glide between their
    // positions, syntax fades out (or in), decorations fade in (or out).
    // Input switches at once; only the drawing takes its time.
    morph_.active = animate && anim::enabled && lay_[0].size() == lines_.size();
    if (morph_.active) {
        morph_.to_reader = r;
        morph_.start = anim::clock;
        morph_.scroll_from = scroll_;
    }
    ghosts_.clear();
    reader_ = r;
    drag_mode_ = 0;
    caret_snap_ = true;
}

double Editor::next_blink(double now) const {
    const double period = 1.1;
    double t = std::fmod(now - activity_, period);
    return now + (t < 0.6 ? 0.6 - t : period - t);
}

// ---------------------------------------------------------------------------
// Layout

Editor::Metrics Editor::metrics(float size) const {
    Metrics m;
    m.size = size;
    m.msize = std::round(size * P_.code_size);
    m.sans = P_.sans->GetFontBaked(size);
    m.mono = P_.mono->GetFontBaked(m.msize);
    m.sans_b = P_.sans_bold ? P_.sans_bold->GetFontBaked(size) : nullptr;
    m.mono_b = P_.mono_bold ? P_.mono_bold->GetFontBaked(m.msize) : nullptr;
    m.tabw = m.sans->GetCharAdvance(' ') * P_.tab_size;
    m.mono_dy = m.sans->Ascent - m.mono->Ascent;
    return m;
}

// The one place that decides which bytes are set in the bold weight, so
// measuring and drawing always agree. Syntax characters stay regular.
bool Editor::bold_of(Style f) const {
    if (syntax_ == Syntax::Ini) return f & I_Section;
    return (f & (S_Bold | S_Heading)) && !(f & (S_Marker | S_Code | S_CodeBlock | S_Tag));
}

float Editor::advance(const Metrics& m, unsigned cp, Style f) const {
    if (cp == '\t') return m.tabw;
    if ((f & S_TaskBox) && !(f & S_Marker)) return std::round(m.sans->Size * 1.3f);  // reader checkbox
    bool b = bold_of(f);
    ImFontBaked* font = is_mono(f) ? (b && m.mono_b ? m.mono_b : m.mono) : (b && m.sans_b ? m.sans_b : m.sans);
    return font->GetCharAdvance((ImWchar)cp);
}

float Editor::measure(const std::string& s, const std::vector<Style>& st, int a, int b, const Metrics& m) const {
    float x = 0;
    const char* p = s.data();
    for (int i = a; i < b;) {
        int len;
        unsigned cp = utf8::decode(p + i, p + s.size(), &len);
        x += advance(m, cp, st.empty() ? 0 : st[i]);
        i += len;
    }
    return x;
}

void Editor::layout(int mode) {
    auto& V = lay_[mode];
    // Same text as last time: no need to hash every line again to find out.
    bool same_text = laid_version_[mode] == version_ && V.size() == lines_.size();
    laid_version_[mode] = version_;
    V.resize(lines_.size());
    bool in_code = false;
    float y = 0, real_y = 0;
    if (mode == 0) {
        drag_real_.clear();
        drag_real_y_.clear();
    }
    for (int i = 0; i < (int)lines_.size(); i++) {
        Line& L = V[i];
        const std::string& s = lines_[i];
        uint64_t hash = same_text ? L.hash : fnv(s);
        if (L.hash != hash || L.in_code != in_code || L.wrap != P_.column || L.base != P_.size ||
            L.lh != P_.line_height || L.tab != P_.tab_size || L.blank != P_.blank_line || L.code != P_.code_size) {
            L.hash = hash;
            L.in_code = in_code;
            L.wrap = P_.column;
            L.base = P_.size;
            L.lh = P_.line_height;
            L.tab = P_.tab_size;
            L.blank = P_.blank_line;
            L.code = P_.code_size;
            if (syntax_ == Syntax::Ini) ini_analyze(s, L.info, L.style);
            else md_analyze(s, in_code, L.info, L.style);
            float scale = L.info.kind == LineKind::Heading ? md_heading_scale(L.info.level) : 1.0f;
            L.size = std::round(P_.size * scale);
            // line_height is in ems, like CSS; headings sit a little tighter.
            // Never shorter than a body line, so small headings keep the rhythm.
            L.row_h = std::max(std::round(P_.size / kEm * P_.line_height),
                               std::round(L.size / kEm * (scale > 1 ? P_.line_height * 0.92f : P_.line_height)));
            if (mode == 1) {
                md_reader(s, L.info, L.style, L.rtext, L.rstyle, L.rmap);
                if (L.info.kind == LineKind::Fence) L.row_h = std::round(L.row_h * 0.5f);
            }
            const std::string& t = mode ? L.rtext : s;
            const std::vector<Style>& st = mode ? L.rstyle : L.style;

            Metrics m = metrics(L.size);

            // Hanging indent: wrapped rows of list items and quotes line up
            // with the text after the marker, indented lines with their indent.
            int hang_to = 0;
            if (syntax_ == Syntax::Markdown && L.info.kind == LineKind::Para) {
                if (L.info.list || L.info.quote) hang_to = L.info.content_start;
                else while (hang_to < (int)s.size() && (s[hang_to] == ' ' || s[hang_to] == '\t')) hang_to++;
                if (mode == 1) {
                    // Same point in the reader text: the first byte at or past it.
                    int r = 0;
                    while (r < (int)L.rmap.size() && L.rmap[r] < hang_to) r++;
                    hang_to = r;
                }
            }
            L.hang = hang_to > 0 ? measure(t, st, 0, hang_to, m) : 0.0f;
            if (L.hang > P_.column * 0.5f) L.hang = 0;

            // Soft wrap at spaces; hard break inside words longer than a row.
            L.rows.assign(1, 0);
            float x = 0, x_brk = 0;
            int row_start = 0, brk = -1;
            const char* p = t.data();
            for (int k = 0; k < (int)t.size();) {
                int len;
                unsigned cp = utf8::decode(p + k, p + t.size(), &len);
                float a = advance(m, cp, st.empty() ? 0 : st[k]);
                float limit = L.rows.size() > 1 ? P_.column - L.hang : P_.column;
                if (cp != ' ' && x + a > limit && k > row_start) {
                    if (brk > row_start) {
                        x -= x_brk;
                        row_start = brk;
                    } else {
                        x = 0;
                        row_start = k;
                    }
                    L.rows.push_back(row_start);
                    brk = -1;
                }
                x += a;
                k += len;
                if (cp == ' ') { brk = k; x_brk = x; }
            }

            // Blank lines are paragraph spacing, not a full line.
            if (L.info.kind == LineKind::Blank) L.row_h = std::max(1.0f, std::round(L.row_h * P_.blank_line));
            L.text_rows = L.rows;
            L.text_row_h = L.row_h;
        }
        // Images are sized every time: they can arrive (or go) on disk, and
        // they're capped by the view's height.
        // While one is carried, the lines it would change are laid out as
        // they'd read once it's dropped (their real tiles kept aside).
        std::string preview;
        bool pv = mode == 0 && L.info.kind == LineKind::Image && drag_preview(i, preview, nullptr);
        float real_h = -1;
        if (pv) {
            layout_image(L, s, i);
            drag_real_[i] = L.tiles;
            real_h = L.rows.size() * L.row_h;
        }
        if (L.info.kind == LineKind::Image) layout_image(L, pv ? preview : s, i);
        else L.img = false;
        L.pad = 0;
        if (L.info.kind == LineKind::Heading && i > 0)
            // Sizes barely step (see md_heading_scale), so the space above
            // carries the level: a top-level section opens widest.
            L.pad = std::round(P_.size * (L.info.level == 1 ? 0.9f : L.info.level == 2 ? 0.5f : 0.25f));
        L.y = y;
        L.h = L.pad + L.rows.size() * L.row_h;
        if (mode == 1) { L.dy = L.y; L.dh = L.h; }  // the reader doesn't animate layout
        if (mode == 0 && img_drag_) {
            drag_real_y_.push_back(real_y);
            real_y += real_h >= 0 ? L.pad + real_h : L.h;
        }
        y += L.h;
        if (L.info.kind == LineKind::Fence) in_code = !in_code;
    }
    if (mode == 0 && img_drag_) drag_real_y_.push_back(real_y);
    total_h_[mode] = y;
}

// Work out which lines survived the latest edit(s), which are new and which
// were removed, by comparing the text now with the text last frame. Must run
// while lay_[0] still holds last frame's layout (before layout(0)).
void Editor::sync_edits() {
    if (version_ == diff_version_) return;
    diff_version_ = version_;
    const std::vector<std::string>& O = prev_lines_;
    const std::vector<std::string>& N = lines_;

    // Unchanged lines at the top and bottom; the edit is in between.
    size_t p = 0;
    while (p < O.size() && p < N.size() && O[p] == N[p]) p++;
    size_t s = 0;
    while (s < O.size() - p && s < N.size() - p && O[O.size() - 1 - s] == N[N.size() - 1 - s]) s++;
    size_t o_end = O.size() - s, n_end = N.size() - s;

    auto finish = [&] {
        carry_layout(p, s);
        prev_lines_ = lines_;
    };
    if (snap_heights_ || reader_ || lay_[0].size() != O.size() || dh_.size() != O.size()) {
        snap_heights_ = true;
        ghosts_.clear();
        finish();
        return;
    }
    if (o_end - p > 200 || n_end - p > 200) {  // a wholesale change: don't animate it
        snap_heights_ = true;
        ghosts_.clear();
        finish();
        return;
    }
    shoot_tiles();  // (the layout still matches the old lines)

    std::vector<int> o2n(O.size(), -1), n2o(N.size(), -1);
    for (size_t i = 0; i < p; i++) o2n[i] = (int)i, n2o[i] = (int)i;
    for (size_t k = 0; k < s; k++) {
        size_t oi = O.size() - 1 - k, ni = N.size() - 1 - k;
        o2n[oi] = (int)ni;
        n2o[ni] = (int)oi;
    }
    // In the middle: identical lines pair first (moved or duplicated lines),
    // then the rest pair up in order (edited lines, e.g. the first half of a
    // split). Whatever is left over was inserted or removed.
    std::unordered_map<std::string, std::vector<int>> pool;
    for (size_t j = n_end; j-- > p;) pool[N[j]].push_back((int)j);
    for (size_t i = p; i < o_end; i++) {
        auto it = pool.find(O[i]);
        if (it == pool.end() || it->second.empty()) continue;
        int j = it->second.back();
        it->second.pop_back();
        o2n[i] = j;
        n2o[j] = (int)i;
    }
    std::vector<int> lo, ln;
    for (size_t i = p; i < o_end; i++) if (o2n[i] < 0) lo.push_back((int)i);
    for (size_t j = p; j < n_end; j++) if (n2o[j] < 0) ln.push_back((int)j);
    for (size_t k = 0; k < lo.size() && k < ln.size(); k++) {
        o2n[lo[k]] = ln[k];
        n2o[ln[k]] = lo[k];
    }

    // Where a removed line (or an older ghost) now sits: above the next line
    // that survived.
    auto before_of = [&](int oi) {
        for (int k = oi; k < (int)O.size(); k++)
            if (o2n[k] >= 0) return o2n[k];
        return (int)N.size();
    };
    for (auto& g : ghosts_) g.before = g.before < (int)O.size() ? before_of(g.before) : (int)N.size();
    for (size_t i = p; i < o_end; i++) {
        if (o2n[i] >= 0) continue;
        Ghost g;
        g.before = before_of((int)i);
        g.lay = lay_[0][i];
        g.text = O[i];
        g.h = dh_[i];
        g.full = std::max(1.0f, lay_[0][i].h);
        // Backspace joining lines: the text lives on in the line above, so
        // only the space collapses (no duplicate of the text fading out).
        g.show_text = !O[i].empty();
        for (size_t j = p; j < n_end && g.show_text; j++)
            if (n2o[j] >= 0 && N[j].find(O[i]) != std::string::npos && N[j] != O[i]) g.show_text = false;
        if (lay_[0][i].img) g.show_text = false;  // its images morph or fade on their own
        g.a = g.show_text ? 1.0f : 0.0f;  // merged lines have no text to fade: close at once
        g.fresh = true;                   // part of this edit's space accounting
        ghosts_.push_back(std::move(g));
    }

    // Lines were added or removed (not just edited in place): conserve the
    // edited region's space. It's settled after the next layout, once the
    // new heights are known (see resolve_pending()).
    bool structural = false;
    for (size_t i = p; i < o_end && !structural; i++) structural = o2n[i] < 0;
    for (size_t j = p; j < n_end && !structural; j++) structural = n2o[j] < 0;
    if (structural) {
        pending_.active = true;
        pending_.begin = (int)p;
        pending_.is_new.assign(n_end - p, 0);
        for (size_t j = p; j < n_end; j++) pending_.is_new[j - p] = n2o[j] < 0;
        pending_.old_space = 0;
        for (size_t i = p; i < o_end; i++) pending_.old_space += dh_[i];
    }

    std::vector<float> dh(N.size(), 0.0f);  // new lines open from nothing
    for (size_t j = 0; j < N.size(); j++)
        if (n2o[j] >= 0) dh[j] = dh_[n2o[j]];
    dh_ = std::move(dh);
    finish();
}

// Layout is cached per line index, so lines added or removed would shift
// every line below them onto another line's cache and all of them would be
// laid out again. The unchanged top (p lines) and bottom (s lines) keep
// their own; the edited middle starts empty. Must run before layout(), while
// the layouts still match prev_lines_ (any that don't are left alone).
void Editor::carry_layout(size_t p, size_t s) {
    const size_t o = prev_lines_.size(), n = lines_.size();
    if (o == n) return;  // nothing shifted
    for (auto& V : lay_) {
        if (V.size() != o) continue;
        V.erase(V.begin() + p, V.begin() + (o - s));
        V.insert(V.begin() + p, n - s - p, Line{});
    }
}

// Settle the space of the last structural edit, now that layout has the new
// heights. The edited region starts with exactly the space it had before:
// lines that carried on (e.g. the first half of a split paragraph) take
// their new height at once, and the difference goes to what's new or
// removed. Breaking a paragraph: the second half inherits the rows its text
// already filled and opens only by the space the break added, so nothing
// moves up. Joining lines: only the space actually freed collapses.
void Editor::resolve_pending() {
    if (!pending_.active) return;
    pending_.active = false;
    auto& V = lay_[0];
    if (dh_.size() != V.size()) return;
    float kept = 0;
    for (size_t k = 0; k < pending_.is_new.size(); k++) {
        int j = pending_.begin + (int)k;
        if (j >= (int)V.size() || pending_.is_new[k]) continue;
        dh_[j] = V[j].h;
        kept += V[j].h;
    }
    float budget = pending_.old_space - kept;  // space the old region had left over
    float ghosts = 0;
    for (auto& g : ghosts_) if (g.fresh) ghosts += g.h;
    if (budget > ghosts) {
        // More room than the removed lines need: new lines start with it.
        float extra = budget - ghosts;
        for (size_t k = 0; k < pending_.is_new.size() && extra > 0; k++) {
            int j = pending_.begin + (int)k;
            if (j >= (int)V.size() || !pending_.is_new[k]) continue;
            float give = std::min(extra, V[j].h);
            dh_[j] = give;
            extra -= give;
        }
    } else if (budget < ghosts) {
        // The carried-on lines grew into some of the removed space: that part
        // is already taken, so it leaves the ghosts now. Merged ghosts first,
        // they have no text to fade.
        float excess = ghosts - budget;
        for (int pass = 0; pass < 2 && excess > 0; pass++)
            for (auto& g : ghosts_) {
                if (!g.fresh || excess <= 0 || g.show_text != (pass == 1)) continue;
                float cut = std::min(excess, g.h);
                g.h -= cut;
                excess -= cut;
            }
    }
    for (auto& g : ghosts_) g.fresh = false;
}

// Displayed heights chase real heights, ghosts collapse, and every line's
// drawn position is the sum of drawn heights above it.
void Editor::animate_layout(float dt) {
    auto& V = lay_[0];
    float geom[5] = {P_.column, P_.size, P_.line_height, P_.blank_line, (float)P_.tab_size};
    bool geom_changed = std::memcmp(geom, geom_, sizeof geom) != 0;
    if (geom_changed) std::memcpy(geom_, geom, sizeof geom);
    if (snap_heights_ || geom_changed || dh_.size() != V.size() || !anim::enabled) {
        dh_.resize(V.size());
        for (size_t i = 0; i < V.size(); i++) dh_[i] = V[i].h;
        ghosts_.clear();
        tile_shots_.clear();
        tile_shots_new_ = false;
        tile_morphs_.clear();
        tile_gone_.clear();
        spacers_.clear();
        snap_heights_ = false;
        if (prev_lines_.size() != lines_.size() || diff_version_ != version_) {
            prev_lines_ = lines_;
            diff_version_ = version_;
        }
    }
    // A carried image headed for a line of its own: room opens where that
    // line would go; wherever it was headed before closes again.
    int want = img_drag_ && drop_changes() && drop_own_ >= 0 ? drop_own_ : -1;
    bool have = false;
    for (auto& s : spacers_) {
        s.target = s.before == want ? drag_own_h_ : 0.0f;
        have = have || s.before == want;
    }
    if (want >= 0 && !have) spacers_.push_back({want, 0.0f, drag_own_h_});
    std::sort(spacers_.begin(), spacers_.end(), [](const Spacer& a, const Spacer& b) { return a.before < b.before; });
    // Where every line (and ghost, and spacer) sits, from the heights as
    // they are now.
    auto place = [&] {
        float y = 0;
        size_t gi = 0, si = 0;
        for (size_t i = 0; i <= V.size(); i++) {
            while (gi < ghosts_.size() && ghosts_[gi].before <= (int)i) {
                ghosts_[gi].dy = y;
                y += ghosts_[gi].h;
                gi++;
            }
            while (si < spacers_.size() && spacers_[si].before <= (int)i) {
                spacers_[si].dy = y;
                y += spacers_[si].h;
                si++;
            }
            if (i == V.size()) break;
            V[i].dy = y;
            V[i].dh = dh_[i];
            y += dh_[i];
        }
    };
    // Images an edit moved start from where they were, measured before the
    // heights take this frame's step: the lines' easing is their own, so
    // an image the edit didn't move doesn't trail the text around it.
    if (tile_shots_new_) {
        std::stable_sort(ghosts_.begin(), ghosts_.end(), [](const Ghost& a, const Ghost& b) { return a.before < b.before; });
        place();
        morph_tiles();
    }
    const float rate = 30.0f;
    for (size_t i = 0; i < V.size(); i++) {
        dh_[i] = anim::approach(dh_[i], V[i].h, rate, dt, 0.3f);
    }
    for (auto& s : spacers_) s.h = anim::approach(s.h, s.target, rate, dt, 0.3f);
    spacers_.erase(std::remove_if(spacers_.begin(), spacers_.end(), [](const Spacer& s) { return s.target <= 0.0f && s.h <= 0.0f; }),
                   spacers_.end());
    // A removed line fades out first, holding its space; only then does the
    // space close. Taking turns means text never overlaps.
    for (auto& g : ghosts_) {
        if (g.a > 0.0f) g.a = anim::approach(g.a, 0.0f, 40.0f, dt, 0.03f);
        else g.h = anim::approach(g.h, 0.0f, rate, dt, 0.3f);
    }
    ghosts_.erase(std::remove_if(ghosts_.begin(), ghosts_.end(), [](const Ghost& g) { return g.h <= 0.0f; }), ghosts_.end());
    std::stable_sort(ghosts_.begin(), ghosts_.end(), [](const Ghost& a, const Ghost& b) { return a.before < b.before; });
    place();
    step_tile_morphs(dt);
}

int Editor::row_of(const Line& L, int col) const {
    int r = (int)(std::upper_bound(L.rows.begin(), L.rows.end(), col) - L.rows.begin()) - 1;
    return std::max(r, 0);
}

int Editor::line_at_y(int mode, float y) const {
    auto& V = lay_[mode];
    int lo = 0, hi = (int)V.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (V[mid].dy <= y) lo = mid;
        else hi = mid - 1;
    }
    return std::max(lo, 0);
}

int Editor::col_at_x(int mode, int line, int row, float x) const {
    const Line& L = lay_[mode][line];
    // Images have places before, between and after them.
    if (L.img) return img_col_at(L, (int)lines_[line].size(), x, L.tiles[0].y + 1);
    const std::string& s = text_of(mode, line);
    const std::vector<Style>& st = style_of(mode, line);
    Metrics m = metrics(L.size);
    int a = L.rows[row];
    int b = row + 1 < (int)L.rows.size() ? L.rows[row + 1] : (int)s.size();
    float cx = row_x(L, row);
    const char* p = s.data();
    for (int i = a; i < b;) {
        int len;
        unsigned cp = utf8::decode(p + i, p + s.size(), &len);
        float w = advance(m, cp, st.empty() ? 0 : st[i]);
        if (x < cx + w * 0.5f) return i;
        cx += w;
        i += len;
    }
    // Past the end of a wrapped row: stay on this row, before the break space.
    if (row + 1 < (int)L.rows.size() && b > a && s[b - 1] == ' ') return b - 1;
    return b;
}

ImVec2 Editor::pos_xy(TextPos p) const {
    const Line& L = lay_[0][p.line];
    if (L.img) {
        float x, y, h;
        img_caret(L, p.col, x, y, h);
        return ImVec2(x, L.dy + L.pad + y);  // the top of the image it's beside
    }
    int r = row_of(L, p.col);
    Metrics m = metrics(L.size);
    float x = row_x(L, r) + measure(lines_[p.line], L.style, L.rows[r], p.col, m);
    return ImVec2(x, L.dy + L.pad + r * L.row_h);
}

bool Editor::hit(int mode, ImVec2 d, int& line, int& col) const {
    auto& V = lay_[mode];
    if (V.empty()) return false;
    if (d.y < 0) { line = 0; col = 0; return true; }
    line = line_at_y(mode, d.y);
    const Line& L = V[line];
    if (d.y >= L.dy + L.dh && line == (int)V.size() - 1) { col = (int)text_of(mode, line).size(); return true; }
    if (L.img) {
        col = img_col_at(L, (int)lines_[line].size(), d.x, d.y - L.dy - L.pad);
        return true;
    }
    int r = std::clamp((int)std::floor((d.y - L.dy - L.pad) / L.row_h), 0, (int)L.rows.size() - 1);
    col = col_at_x(mode, line, r, d.x);
    return true;
}

// ---------------------------------------------------------------------------
// Movement

void Editor::move_to(TextPos p, bool select) {
    clamp(p);
    cur_ = p;
    if (!select) anchor_ = p;
    reveal_ = true;
    reveal_mode_ = 0;
    activity_ = ImGui::GetTime();
}

TextPos Editor::left_of(TextPos p, bool word) const {
    const std::string& s = lines_[p.line];
    if (p.col == 0) return p.line > 0 ? TextPos{p.line - 1, (int)lines_[p.line - 1].size()} : p;
    if (is_img(p.line)) {  // over an image in one step
        const Line& L = lay_[0][p.line];
        return {p.line, stop_col(L, img_stop(L, p.col) - 1, (int)s.size())};
    }
    if (!word) return {p.line, utf8::prev(s, p.col)};
    int i = p.col;
    while (i > 0 && char_class(s[i - 1]) == 0) i--;
    if (i > 0) {
        int cls = char_class(s[i - 1]);
        while (i > 0 && char_class(s[i - 1]) == cls) i--;
    }
    return {p.line, i};
}

TextPos Editor::right_of(TextPos p, bool word) const {
    const std::string& s = lines_[p.line];
    if (p.col >= (int)s.size()) return p.line + 1 < (int)lines_.size() ? TextPos{p.line + 1, 0} : p;
    if (is_img(p.line)) {
        const Line& L = lay_[0][p.line];
        return {p.line, stop_col(L, img_stop(L, p.col) + 1, (int)s.size())};
    }
    if (!word) return {p.line, utf8::next(s, p.col)};
    int i = p.col, n = (int)s.size();
    while (i < n && char_class(s[i]) == 0) i++;
    if (i < n) {
        int cls = char_class(s[i]);
        while (i < n && char_class(s[i]) == cls) i++;
    }
    return {p.line, i};
}

TextPos Editor::smart_home() const {
    const std::string& s = lines_[cur_.line];
    LineInfo info;
    std::vector<Style> st;
    md_analyze(s, in_code_before(cur_.line), info, st);
    int first = 0;
    while (first < (int)s.size() && (s[first] == ' ' || s[first] == '\t')) first++;
    int content = (info.list || info.quote || info.kind == LineKind::Heading) ? info.content_start : first;
    // Visual row start first, then content start, then column 0.
    if (!lay_[0].empty() && cur_.line < (int)lay_[0].size()) {
        const Line& L = lay_[0][cur_.line];
        int r = row_of(L, cur_.col);
        if (r > 0 && cur_.col != L.rows[r]) return {cur_.line, L.rows[r]};
    }
    return {cur_.line, cur_.col == content ? 0 : content};
}

TextPos Editor::row_end() const {
    const std::string& s = lines_[cur_.line];
    if (!lay_[0].empty() && cur_.line < (int)lay_[0].size()) {
        const Line& L = lay_[0][cur_.line];
        int r = row_of(L, cur_.col);
        if (r + 1 < (int)L.rows.size()) {
            int e = L.rows[r + 1];
            if (e > 0 && s[e - 1] == ' ') e--;
            if (cur_.col != e) return {cur_.line, e};
        }
    }
    return {cur_.line, (int)s.size()};
}

void Editor::move_vertical(int n, bool select) {
    auto& V = lay_[0];
    if (V.size() != lines_.size()) return;
    if (want_x_ < 0) want_x_ = pos_xy(cur_).x;
    int line = cur_.line;
    int r = row_of(V[line], cur_.col);
    TextPos target{-1, -1};
    for (int k = 0; k < std::abs(n); k++) {
        if (n > 0) {
            if (r + 1 < (int)V[line].rows.size()) r++;
            else if (line + 1 < (int)V.size()) { line++; r = 0; }
            else { target = {line, (int)lines_[line].size()}; break; }
        } else {
            if (r > 0) r--;
            else if (line > 0) { line--; r = (int)V[line].rows.size() - 1; }
            else { target = {0, 0}; break; }
        }
    }
    float keep = want_x_;
    if (target.line < 0 && !select && line != cur_.line && is_img(line)) {  // onto an image: the nearest one
        const Line& L = V[line];
        pick(line, nearest_tile(L, want_x_, n < 0 ? L.tiles.back().y : L.tiles.front().y));
        want_x_ = keep;
        return;
    }
    if (target.line < 0) target = {line, col_at_x(0, line, r, want_x_)};
    move_to(target, select);
    want_x_ = keep;
}

void Editor::select_word_at(TextPos p) {
    const std::string& s = lines_[p.line];
    int n = (int)s.size();
    if (is_img(p.line)) {  // an image is one word
        anchor_ = {p.line, 0};
        cur_ = {p.line, n};
        return;
    }
    int a = p.col, b = p.col;
    int cls = a < n ? char_class(s[a]) : (a > 0 ? char_class(s[a - 1]) : 0);
    while (a > 0 && char_class(s[a - 1]) == cls) a--;
    while (b < n && char_class(s[b]) == cls) b++;
    anchor_ = {p.line, a};
    cur_ = {p.line, b};
}

// ---------------------------------------------------------------------------
// Typing

void Editor::type_char(unsigned c) {
    std::string s;
    utf8::append(s, c);
    static const char* pairs[][2] = {{"*", "*"}, {"_", "_"}, {"`", "`"}, {"~", "~"}, {"=", "="},
                                     {"(", ")"}, {"[", "]"}, {"\"", "\""}};
    if (has_selection() && cur_.line == anchor_.line && !is_img(cur_.line)) {
        for (auto& p : pairs) {
            if (s != p[0]) continue;
            begin_edit(Edit::Other);
            bool fwd = anchor_ < cur_;
            TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
            lines_[a.line].insert(b.col, p[1]);
            lines_[a.line].insert(a.col, p[0]);
            a.col += 1;
            b.col += 1;
            anchor_ = fwd ? a : b;
            cur_ = fwd ? b : a;
            end_edit();
            return;
        }
    }
    begin_edit(has_selection() ? Edit::Other : Edit::Type);
    step_off_image();
    replace_selection(s);
    end_edit();
}

void Editor::newline() {
    if (picked() >= 0) {  // a line of its own after the image
        begin_edit(Edit::Other);
        step_off_image();
        end_edit();
        return;
    }
    begin_edit(Edit::Other);
    delete_selection();
    const std::string s = lines_[cur_.line];
    LineInfo info;
    std::vector<Style> st;
    md_analyze(s, in_code_before(cur_.line), info, st);

    std::string prefix;
    bool plain = info.kind == LineKind::Para || info.kind == LineKind::Blank;
    if (plain && (info.list || info.quote) && cur_.col >= info.content_start) {
        bool empty = trim(s.substr(info.content_start)).empty();
        if (empty) {
            // Enter on an empty item ends the list instead of adding another.
            std::string keep = s.substr(0, info.list ? info.marker_begin : 0);
            if (trim(keep).empty() || !info.list) keep.clear();
            else keep = s.substr(0, info.marker_begin);
            lines_[cur_.line] = keep;
            cur_ = anchor_ = {cur_.line, (int)keep.size()};
            end_edit();
            return;
        }
        prefix = s.substr(0, info.marker_begin);
        if (info.list) {
            if (info.ordered) {
                int d = info.marker_begin;
                while (d < (int)s.size() && std::isdigit((unsigned char)s[d])) d++;
                long num = std::strtol(s.c_str() + info.marker_begin, nullptr, 10);
                prefix += std::to_string(num + 1) + s[d] + " ";
            } else {
                prefix += s.substr(info.marker_begin, 2);
            }
            if (info.task) prefix += "[ ] ";
        }
    } else {
        int k = 0;
        while (k < (int)s.size() && (s[k] == ' ' || s[k] == '\t')) k++;
        if (cur_.col >= k) prefix = s.substr(0, k);
    }
    replace_selection("\n" + prefix);
    renumber_lists(cur_.line - 1, cur_.line);
    end_edit();
}

void Editor::backspace(bool word) {
    int tile = 0;
    if (int line = picked(&tile); line >= 0) { remove_picked(line, tile, -1); return; }
    if (has_selection()) {
        begin_edit(Edit::Other);
        delete_selection();
        end_edit();
        return;
    }
    if (cur_.line == 0 && cur_.col == 0) return;
    if (image_backspace()) return;
    begin_edit(Edit::Delete);
    const std::string& s = lines_[cur_.line];
    TextPos from;
    if (cur_.col == 0) {
        from = {cur_.line - 1, (int)lines_[cur_.line - 1].size()};
    } else {
        LineInfo info;
        std::vector<Style> st;
        md_analyze(s, in_code_before(cur_.line), info, st);
        int lead = 0;
        while (lead < (int)s.size() && s[lead] == ' ') lead++;
        if (info.list && cur_.col == info.content_start && !word) {
            from = {cur_.line, info.marker_begin};  // backspace at item start drops the marker
        } else if (word) {
            from = left_of(cur_, true);
        } else if (cur_.col <= lead && cur_.col % P_.tab_size == 0) {
            from = {cur_.line, cur_.col - P_.tab_size};
        } else {
            from = {cur_.line, utf8::prev(s, cur_.col)};
        }
    }
    erase(from, cur_);
    cur_ = anchor_ = from;
    renumber_lists(cur_.line, cur_.line);
    end_edit();
}

void Editor::del(bool word) {
    int tile = 0;
    if (int line = picked(&tile); line >= 0) { remove_picked(line, tile, 1); return; }
    if (has_selection()) {
        begin_edit(Edit::Other);
        delete_selection();
        end_edit();
        return;
    }
    if (image_delete()) return;
    TextPos to = right_of(cur_, word);
    if (to == cur_) return;
    begin_edit(Edit::Delete);
    erase(cur_, to);
    anchor_ = cur_;
    end_edit();
}

void Editor::tab() {
    if (picked() >= 0) return;  // (it would replace the image)
    LineInfo info;
    std::vector<Style> st;
    md_analyze(lines_[cur_.line], in_code_before(cur_.line), info, st);
    if ((has_selection() && cur_.line != anchor_.line) || (info.list && !has_selection())) {
        indent();
        return;
    }
    begin_edit(Edit::Other);
    int n = P_.tab_size - (cur_.col % P_.tab_size);
    replace_selection(std::string(n, ' '));
    end_edit();
}

void Editor::handle_keys() {
    ImGuiIO& io = ImGui::GetIO();
    bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;

    // The completion list takes the keys it uses while it's up. Enter on the
    // word exactly as typed takes nothing: the line breaks as usual.
    ImGuiKey taken[6] = {};
    int n_taken = 0;
    if (comp_.open && !ctrl && !alt && !shift && !comp_.items.empty()) {
        int n = (int)comp_.items.size();
        auto take = [&](ImGuiKey k) { taken[n_taken++] = k; };
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { comp_.sel = (comp_.sel + 1) % n; take(ImGuiKey_DownArrow); }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) { comp_.sel = (comp_.sel - 1 + n) % n; take(ImGuiKey_UpArrow); }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            comp_dismissed_line_ = comp_.line;
            comp_dismissed_start_ = comp_.start;
            comp_.open = false;
            take(ImGuiKey_Escape);
        }
        bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        if (enter && comp_.open) {
            if (comp_.items[comp_.sel].insert == comp_.word) {
                comp_.open = false;
            } else {
                accept_completion(comp_.sel);
                take(ImGuiKey_Enter);
                take(ImGuiKey_KeypadEnter);
            }
        }
    }
    auto K = [&](ImGuiKey k) {
        return ImGui::IsKeyPressed(k, true) && std::find(taken, taken + n_taken, k) == taken + n_taken;
    };

    // ctrl+alt+left/right: the image the caret is on swaps places with its
    // neighbour in the row (as alt+up/down moves lines).
    if (ctrl && alt && !shift) {
        if (K(ImGuiKey_LeftArrow)) nudge_image(-1);
        if (K(ImGuiKey_RightArrow)) nudge_image(1);
    }
    if (!alt) {
        int pk_tile = 0;
        int pk = shift ? -1 : picked(&pk_tile);  // arrows step image by image
        if (K(ImGuiKey_LeftArrow)) {
            if (pk >= 0) step_picked(pk, pk_tile, -1);
            else if (has_selection() && !shift) move_to(std::min(cur_, anchor_), false);
            else move_to(left_of(cur_, ctrl), shift);
        }
        if (K(ImGuiKey_RightArrow)) {
            if (pk >= 0) step_picked(pk, pk_tile, 1);
            else if (has_selection() && !shift) move_to(std::max(cur_, anchor_), false);
            else move_to(right_of(cur_, ctrl), shift);
        }
        float row = std::round(P_.size / kEm * P_.line_height);
        if (K(ImGuiKey_UpArrow)) {
            if (ctrl) { move_to(paragraph_move(-1), shift); want_x_ = -1; }  // previous paragraph
            else if (pk >= 0) vertical_picked(pk, pk_tile, -1);
            else move_vertical(-1, shift);
        }
        if (K(ImGuiKey_DownArrow)) {
            if (ctrl) { move_to(paragraph_move(1), shift); want_x_ = -1; }  // next paragraph
            else if (pk >= 0) vertical_picked(pk, pk_tile, 1);
            else move_vertical(1, shift);
        }
        int page = std::max(1, (int)(view_h_ / row) - 2);
        if (K(ImGuiKey_PageUp)) { scroll_target_ -= page * row; move_vertical(-page, shift); }
        if (K(ImGuiKey_PageDown)) { scroll_target_ += page * row; move_vertical(page, shift); }
        if (K(ImGuiKey_Home)) {
            if (ctrl) move_to({0, 0}, shift);
            else move_to(smart_home(), shift);
            want_x_ = -1;
        }
        if (K(ImGuiKey_End)) {
            if (ctrl) move_to({(int)lines_.size() - 1, (int)lines_.back().size()}, shift);
            else move_to(row_end(), shift);
            want_x_ = -1;
        }
        if (!shift || !ctrl) {
            if (K(ImGuiKey_Backspace)) backspace(ctrl);
            if (K(ImGuiKey_Delete)) del(ctrl);
        }
        if (!ctrl && (K(ImGuiKey_Enter) || K(ImGuiKey_KeypadEnter))) newline();
        if (!ctrl && K(ImGuiKey_Tab)) {
            if (shift) outdent();
            else tab();
        }
        if (K(ImGuiKey_Escape)) anchor_ = cur_;
    }

    // Text. Ctrl+Alt is AltGr on many layouts, so only plain Ctrl or plain
    // Alt suppress typing.
    if (ctrl == alt) {
        for (ImWchar c : io.InputQueueCharacters)
            if (c >= 32 && c != 127) type_char(c);
    }
    io.InputQueueCharacters.resize(0);
}

void Editor::handle_mouse(bool hovered, bool active, ImVec2 d) {
    ImGuiIO& io = ImGui::GetIO();
    int mode = reader_ ? 1 : 0;
    int line, col;
    // The completion list: a click takes a choice; anywhere else puts it away.
    if (comp_.open && hovered) {
        ImVec2 m = io.MousePos;
        bool over = m.x >= comp_.box_min.x && m.x < comp_.box_max.x && m.y >= comp_.box_min.y && m.y < comp_.box_max.y;
        if (over) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int k = comp_.top + (int)std::floor((m.y - comp_.box_min.y - comp_.pad_y) / std::max(1.0f, comp_.row_h));
            if (over && k >= 0 && k < (int)comp_.items.size()) {
                accept_completion(k);
                return;
            }
            comp_.open = false;
        }
        if (over) return;
    }
    if (!reader_ && handle_image_mouse(hovered, d)) return;

    // Links: ctrl+click in the editor, plain click in the reader.
    bool link_mod = reader_ || io.KeyCtrl;
    const LinkSpan* hover_link = nullptr;
    static std::vector<LinkSpan> links;
    int src_col = -1, hit_line = -1;
    bool on_box = false;
    if (hovered && hit(mode, d, line, col)) {
        hit_line = line;
        const Line& L = lay_[mode][line];
        if (mode == 1) {
            if (col < (int)L.rmap.size()) src_col = L.rmap[col];
            if (col < (int)L.rstyle.size() && (L.rstyle[col] & S_TaskBox)) on_box = true;
        } else {
            src_col = col;
        }
        if (src_col >= 0) {
            links.clear();
            LineInfo info;
            std::vector<Style> st;
            md_analyze(lines_[line], lay_[0].size() == lines_.size() ? lay_[0][line].in_code : in_code_before(line), info, st, &links);
            for (auto& l : links)
                if (src_col >= l.begin && src_col < l.end) hover_link = &l;
        }
    }
    if (hovered) {
        if ((hover_link && link_mod) || on_box) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (!reader_) ImGui::SetMouseCursor(hover_tile_ >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_TextInput);
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hover_link && link_mod) {
            if (on_link) on_link(*hover_link);
            drag_mode_ = 0;
            return;
        }
        if (reader_) {
            if (on_box && hit_line >= 0) {
                begin_edit(Edit::Other);
                toggle_task_line(hit_line);
                end_edit(false);
            }
            return;
        }
        if (hit(0, d, line, col)) {
            TextPos p{line, col};
            int clicks = io.MouseClickedCount[0];
            if (clicks == 2) {
                select_word_at(p);
                drag_mode_ = 2;
            } else if (clicks >= 3) {
                anchor_ = {line, 0};
                cur_ = line + 1 < (int)lines_.size() ? TextPos{line + 1, 0} : TextPos{line, (int)lines_[line].size()};
                drag_mode_ = 3;
            } else {
                if (io.KeyShift) cur_ = p;
                else cur_ = anchor_ = p;
                drag_mode_ = 1;
            }
            drag_a_ = std::min(cur_, anchor_);
            drag_b_ = std::max(cur_, anchor_);
            want_x_ = -1;
            activity_ = ImGui::GetTime();
        }
    }

    if (!reader_ && active && drag_mode_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        if (hit(0, d, line, col)) {
            TextPos p{line, col};
            if (drag_mode_ == 1) {
                cur_ = p;
            } else {
                TextPos wa = p, wb = p;
                if (drag_mode_ == 2) {
                    TextPos sa = cur_, sb = anchor_;
                    select_word_at(p);
                    wa = anchor_;
                    wb = cur_;
                    cur_ = sa;
                    anchor_ = sb;
                } else {
                    wa = {line, 0};
                    wb = line + 1 < (int)lines_.size() ? TextPos{line + 1, 0} : TextPos{line, (int)lines_[line].size()};
                }
                if (p < drag_a_) { anchor_ = drag_b_; cur_ = wa; }
                else { anchor_ = drag_a_; cur_ = std::max(wb, drag_b_); }
            }
            activity_ = ImGui::GetTime();
            reveal_ = true;
            reveal_mode_ = 0;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_mode_ = 0;
}

// ---------------------------------------------------------------------------
// Completion

void Editor::update_completion(bool typed) {
    if (!comp_.open && !typed) return;
    if (reader_ || has_selection() || lay_[0].size() != lines_.size() || completers_.empty()) {
        comp_.open = false;
        return;
    }
    const std::string& s = lines_[cur_.line];
    int src = -1, start = -1;
    for (int k = 0; k < (int)completers_.size() && src < 0; k++) {
        const Completer& c = completers_[k];
        if (c.syntax != syntax_ || !c.trigger || !c.items) continue;
        int at = c.trigger(s, cur_.col, lay_[0][cur_.line].style);
        if (at >= 0 && at <= cur_.col) src = k, start = at;
    }
    if (src < 0) {
        comp_.open = false;
        comp_dismissed_line_ = -1;  // out of the word: escape's is over
        return;
    }
    bool same_word = comp_.open && src == comp_.src && cur_.line == comp_.line && start == comp_.start;
    // Moving the caret (not typing) never brings it up, nor carries it to another word.
    if (!typed && !same_word) { comp_.open = false; return; }
    if (cur_.line == comp_dismissed_line_ && start == comp_dismissed_start_) { comp_.open = false; return; }
    std::string word = s.substr(start, cur_.col - start);
    if ((int)word.size() < completers_[src].min_chars) { comp_.open = false; return; }
    if (same_word && word == comp_.word) return;

    std::vector<CompletionItem> items = completers_[src].items(word);
    // Nothing to offer, or only what's already there.
    if (items.empty() || (items.size() == 1 && items[0].insert == word)) { comp_.open = false; return; }
    std::string keep = comp_.open && comp_.sel < (int)comp_.items.size() ? comp_.items[comp_.sel].label : "";
    if (!comp_.open) {
        comp_.a = 0;
        comp_.top = 0;
        comp_.sel_y = 0;
    }
    comp_.open = true;
    comp_.src = src;
    comp_.line = cur_.line;
    comp_.start = start;
    comp_.word = word;
    comp_.items = std::move(items);
    comp_.sel = 0;
    for (int k = 0; k < (int)comp_.items.size() && !keep.empty(); k++)
        if (comp_.items[k].label == keep) { comp_.sel = k; break; }
}

void Editor::accept_completion(int k) {
    comp_.open = false;
    if (k < 0 || k >= (int)comp_.items.size() || comp_.src < 0 || cur_.line != comp_.line || comp_.start > cur_.col) return;
    const Completer& c = completers_[comp_.src];
    begin_edit(Edit::Other);
    unpick();
    TextPos a{comp_.line, comp_.start};
    erase(a, cur_);
    TextPos e = insert(a, comp_.items[k].insert);
    if (!c.close.empty()) {
        if (lines_[e.line].compare(e.col, c.close.size(), c.close) == 0) e.col += (int)c.close.size();
        else e = insert(e, c.close);
    }
    cur_ = anchor_ = e;
    end_edit();
}

// Under the word, as vim sets it: rows of mono text in a block sunk into
// the page, the pick's row lit, what the word matched in bold, and a detail
// beside each; a thumb at the right edge when there are more than fit. The
// labels line up with the word; above its line when there's no room below.
void Editor::draw_completion(ImDrawList* dl, float dt) {
    if (!comp_.open || reader_ || comp_.items.empty() || comp_.line >= (int)lay_[0].size()) return;
    const Completer& c = completers_[comp_.src];
    ImFont* font = P_.mono ? P_.mono : P_.sans;
    ImFont* bold = P_.mono_bold ? P_.mono_bold : font;
    const float fs = std::round(P_.size * P_.ui_size);
    auto W = [&](ImFont* f, const std::string& t) { return f->CalcTextSizeA(fs, FLT_MAX, 0, t.c_str()).x; };
    const float dpi = P_.dpi;
    const float row_h = std::round(fs * 1.45f), pad_y = 0;  // rows flush with the block: a lit first or last row fills its end
    const int n = (int)comp_.items.size();
    const int rows = std::min(n, 8);

    comp_.sel = std::clamp(comp_.sel, 0, n - 1);
    int top_was = comp_.top;
    if (comp_.sel < comp_.top) comp_.top = comp_.sel;
    if (comp_.sel >= comp_.top + rows) comp_.top = comp_.sel - rows + 1;
    comp_.top = std::clamp(comp_.top, 0, n - rows);
    comp_.a = anim::approach(comp_.a, 1.0f, 30.0f, dt, 0.01f);
    // The lit row glides between rows, but the rows scrolling under it
    // would carry it backwards first: then it's just there.
    if (comp_.a < 0.05f || comp_.top != top_was) comp_.sel_y = (float)comp_.sel;
    else comp_.sel_y = anim::approach(comp_.sel_y, (float)comp_.sel, 45.0f, dt, 0.01f);

    // A label as the palette sets it: matched characters in the bold
    // weight, its first `dim` bytes (a link's "space/") quieter, and cut
    // short with an ellipsis past `max_w`. Returns the width it takes.
    const std::string ellipsis = "\xE2\x80\xA6";  // "…"
    auto label = [&](const CompletionItem& it, float lx, float ly, ImU32 base, ImU32 hl, ImU32 quiet, float max_w,
                     bool draw) {
        const std::string& s = it.label;
        std::vector<char> kind(s.size(), 0);  // 0 plain, 1 matched, 2 quiet
        for (int k = 0; k < std::min(it.dim, (int)s.size()); k++) kind[k] = 2;
        for (int b : it.hl)
            for (int e = utf8::next(s, b), i = b; i < e && i >= 0; i++) kind[i] = 1;
        auto font_of = [&](size_t i) { return kind[i] == 1 ? bold : font; };
        float total = 0;
        for (size_t i = 0; i < s.size(); i = utf8::next(s, (int)i))
            total += font_of(i)->CalcTextSizeA(fs, FLT_MAX, 0, s.data() + i, s.data() + utf8::next(s, (int)i)).x;
        size_t cut = s.size();
        bool cut_short = total > max_w;
        if (cut_short) {
            float room = max_w - W(font, ellipsis), w = 0;
            for (cut = 0; cut < s.size();) {
                size_t e = utf8::next(s, (int)cut);
                float cw = font_of(cut)->CalcTextSizeA(fs, FLT_MAX, 0, s.data() + cut, s.data() + e).x;
                if (w + cw > room) break;
                w += cw;
                cut = e;
            }
        }
        if (!draw) return std::min(total, max_w);
        float x = lx;
        for (size_t i = 0; i < cut;) {
            size_t j = i;
            while (j < cut && kind[j] == kind[i]) j++;
            ImFont* f = font_of(i);
            dl->AddText(f, fs, ImVec2(std::round(x), ly), kind[i] == 1 ? hl : kind[i] == 2 ? quiet : base, s.data() + i, s.data() + j);
            x += f->CalcTextSizeA(fs, FLT_MAX, 0, s.data() + i, s.data() + j).x;
            i = j;
        }
        if (cut_short) {
            dl->AddText(font, fs, ImVec2(std::round(x), ly), base, ellipsis.c_str());
            x += W(font, ellipsis);
        }
        return x - lx;
    };

    float label_w = 0, meta_w = 0;
    for (auto& it : comp_.items) {
        label_w = std::max(label_w, label(it, 0, 0, 0, 0, 0, FLT_MAX, false));
        if (!it.meta.empty()) meta_w = std::max(meta_w, W(font, it.meta));
    }
    const float gap = W(font, "  ");
    const float margin = std::round(fs * 0.6f);  // the block's side padding
    const float h = rows * row_h + pad_y * 2;
    // The labels' text lines up with the word (a tag's with its #).
    const Line& L = lay_[0][comp_.line];
    int lead_col = std::max(0, comp_.start - c.lead);
    ImVec2 at = pos_xy({comp_.line, lead_col});
    float row_top = origin_y_ + at.y + yoff_[comp_.line];
    float lx = std::round(col_x_ + at.x);

    // No further right than the text column (from a word far along its
    // line, half of it at least): long labels end in "…".
    const float meta_part = meta_w > 0 ? gap + meta_w : 0;
    const float room = std::max(col_x_ + P_.column - lx, P_.column * 0.5f) - margin;
    label_w = std::max(std::min(label_w, room - meta_part), fs * 4);
    float list_w = label_w + meta_part;
    lx = std::max(P_.min.x + margin, std::min(lx, P_.max.x - margin - list_w));
    float space = std::round(2 * dpi);
    float y = row_top + L.row_h + space;
    if (y + h > P_.max.y && row_top - space - h >= P_.min.y) y = row_top - space - h;
    y = std::round(y);

    // A block a shade darker than the page, sunk into it rather than
    // floating over it, cornered like the caret and the palette's bar.
    float x0 = std::round(lx - margin), x1 = std::round(lx + list_w + margin);
    comp_.box_min = ImVec2(x0, y);
    comp_.box_max = ImVec2(x1, y + h);
    comp_.row_h = row_h;
    comp_.pad_y = pad_y;
    auto col = [&](ImU32 c0) { return fade(col_alpha(c0, comp_.a)); };
    const float corner = std::round(dpi);
    ImU32 sunk = col_mix(T.bg, IM_COL32(0, 0, 0, 255), T.dark ? 0.3f : 0.04f);
    dl->AddRectFilled(comp_.box_min, comp_.box_max, col(sunk), corner);

    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + h), true);
    const float ty = std::round((row_h - fs) * 0.5f);
    const float sy = std::round(y + pad_y + (comp_.sel_y - comp_.top) * row_h);
    dl->AddRectFilled(ImVec2(x0, sy), ImVec2(x1, sy + row_h), col(T.selection), corner);
    for (int k = comp_.top; k < comp_.top + rows; k++) {
        const CompletionItem& it = comp_.items[k];
        float ry = y + pad_y + (k - comp_.top) * row_h;
        bool on = k == comp_.sel;
        ImU32 quiet = col(on ? col_mix(T.dim, T.text, 0.45f) : T.dim);
        label(it, lx, ry + ty, col(on ? T.text : col_mix(T.dim, T.text, 0.55f)), col(on ? T.bright : T.text), quiet, label_w, true);
        if (!it.meta.empty())
            dl->AddText(font, fs, ImVec2(lx + label_w + gap + meta_w - W(font, it.meta), ry + ty), quiet, it.meta.c_str());
    }
    // More than fit: a thumb flush with the right edge, as long as the
    // share showing, where the rows showing are.
    if (n > rows) {
        float tw = std::max(2.0f, std::round(3 * dpi));
        float th = std::max(std::round(h * rows / n), std::round(row_h * 0.5f));
        float tt = std::round(y + (h - th) * comp_.top / (n - rows));
        dl->AddRectFilled(ImVec2(x1 - tw, tt), ImVec2(x1, tt + th), col(T.faint), corner);
    }
    dl->PopClipRect();
}

void Editor::rewrite(const std::vector<std::string>& lines, TextPos cur) {
    std::vector<std::string> before = lines_;
    begin_edit(Edit::Other);
    lines_ = lines.empty() ? std::vector<std::string>{""} : lines;
    clamp(cur);
    cur_ = anchor_ = cur;
    last_edit_ = Edit::None;
    end_edit(false);
    pulse_changes(before);
}

// ---------------------------------------------------------------------------
// Drawing

ImU32 Editor::fade(ImU32 c) const { return col_alpha(c, P_.alpha); }

void Editor::draw(const EditorParams& p) {
    P_ = p;
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    double now = ImGui::GetTime();
    int mode = reader_ ? 1 : 0;
    view_h_ = p.max.y - p.min.y;
    col_x_ = std::round(p.min.x + (p.max.x - p.min.x - p.column) * 0.5f);

    // Problems fade in and out.
    marks_snap_ = false;
    for (auto& m : marks_) m.a = anim::approach(m.a, m.live ? 1.0f : 0.0f, 16.0f, dt, 0.004f);
    marks_.erase(std::remove_if(marks_.begin(), marks_.end(), [](const Mark& m) { return !m.live && m.a <= 0.0f; }), marks_.end());

    // A carried image's preview changed (last frame's input): what's drawn
    // now is where the images morph from.
    if (int sig = drag_signature(); sig != drag_sig_) {
        if (!reader_ && lay_[0].size() == lines_.size()) shoot_tiles();
        drag_sig_ = sig;
    }
    auto relayout = [&] {
        sync_edits();
        layout(0);
        resolve_pending();
        if (mode || morph_.active) layout(1);
        if (yoff_.size() != lines_.size()) yoff_.assign(lines_.size(), 0);
    };
    relayout();

    if (sync_line_ >= 0 && sync_line_ < (int)lay_[mode].size()) {
        const Line& L = lay_[mode][sync_line_];
        scroll_ = scroll_target_ = L.y + sync_frac_ * L.h + p.top_pad;
        sync_line_ = -1;
    }

    // Input. The invisible button owns the mouse for the whole area so
    // drags keep tracking outside the column.
    ImGui::SetCursorScreenPos(p.min);
    ImGui::InvisibleButton("##editor", ImVec2(p.max.x - p.min.x, p.max.y - p.min.y));
    bool hovered = p.mouse && ImGui::IsItemHovered();
    bool active = p.mouse && ImGui::IsItemActive();

    if (hovered && io.MouseWheel != 0) {
        scroll_target_ -= io.MouseWheel * std::round(p.size / kEm * p.line_height) * 3;
        scroll_activity_ = now;
    }

    auto doc_of = [&](ImVec2 m) { return ImVec2(m.x - col_x_, m.y - (p.min.y + p.top_pad - scroll_)); };
    uint64_t before = version_;
    if (p.mouse) handle_mouse(hovered, active, doc_of(io.MousePos));
    if (p.keyboard && !reader_) handle_keys();
    if (p.keyboard && reader_) {
        float row = std::round(p.size / kEm * p.line_height);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) scroll_target_ -= row * 2;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) scroll_target_ += row * 2;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) scroll_target_ -= view_h_ * 0.85f;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_Space)) scroll_target_ += view_h_ * 0.85f;
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) scroll_target_ = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) scroll_target_ = 1e9f;
        io.InputQueueCharacters.resize(0);
    }
    if (version_ != before) relayout();
    settle_on_images();
    // The completion list follows typing; it goes while the keys are elsewhere.
    if (!p.keyboard) comp_.open = false;
    else update_completion(version_ != before && (last_edit_ == Edit::Type || last_edit_ == Edit::Delete));

    animate_layout(dt);

    // Auto-scroll while drag-selecting past the edges.
    if (active && drag_mode_ && !reader_) {
        float my = io.MousePos.y;
        if (my < p.min.y + 8) scroll_target_ -= (p.min.y + 8 - my) * dt * 12;
        if (my > p.max.y - 8) scroll_target_ += (my - p.max.y + 8) * dt * 12;
    }

    // Keep the caret in view after keyboard moves.
    float content_h = p.top_pad + total_h_[mode] + (p.scroll_past_end ? view_h_ * 0.5f : p.top_pad);
    float max_scroll = std::max(0.0f, content_h - view_h_);
    if (reveal_ && !reader_) {
        ImVec2 c = pos_xy(cur_);
        const Line& L = lay_[0][cur_.line];
        float row_top = c.y + p.top_pad, row_bot = row_top + L.row_h;
        if (L.img) {
            // An image line: the picked image itself (or the one the caret's
            // by), not the line's whole height.
            int tile = 0;
            if (picked(&tile) != cur_.line) tile = img_tile(L, cur_.col);
            const Line::Tile& t = L.tiles[tile];
            row_top = L.dy + L.pad + t.y + p.top_pad;
            row_bot = row_top + t.h;
        }
        if (L.img && reveal_mode_ == 0) {
            // A text line's margin (an image is tall enough as it is); bottom
            // first, so one taller than the view shows its top.
            float margin = std::round(p.size / kEm * p.line_height) * 1.5f;
            if (row_bot + margin > scroll_target_ + view_h_) scroll_target_ = row_bot + margin - view_h_;
            if (row_top - margin < scroll_target_) scroll_target_ = row_top - margin;
        } else if (reveal_mode_ == 1) {
            scroll_target_ = L.y + p.top_pad - view_h_ * 0.22f;
        } else if (reveal_mode_ == 2) {
            scroll_target_ = row_top - view_h_ * 0.4f;
        } else if (reveal_mode_ == 3) {
            // Jumps (find, headings): leave the view alone if the target is
            // comfortably visible; otherwise bring it to the upper-middle.
            float margin = std::min(L.row_h * 2.0f, view_h_ * 0.2f);
            if (row_top - margin < scroll_target_ || row_bot + margin > scroll_target_ + view_h_)
                scroll_target_ = row_top - view_h_ * 0.4f;
        } else {
            float margin = std::min(L.row_h * 2.5f, view_h_ * 0.25f);
            if (row_top - margin < scroll_target_) scroll_target_ = row_top - margin;
            if (row_bot + margin > scroll_target_ + view_h_) scroll_target_ = row_bot + margin - view_h_;
        }
        reveal_ = false;
        reveal_mode_ = 0;
    }
    scroll_target_ = std::clamp(scroll_target_, 0.0f, max_scroll);
    if (snap_) { scroll_ = scroll_target_; snap_ = false; }
    float prev_scroll = scroll_;
    scroll_ = anim::approach(scroll_, scroll_target_, 26.0f, dt, 0.4f);
    if (std::fabs(prev_scroll - scroll_) > 0.5f) scroll_activity_ = now;

    for (float& o : yoff_) o = anim::approach(o, 0.0f, 30.0f, dt, 0.4f);

    // Markdown syntax is fully visible only on the lines you're working on
    // (the caret's line, or every line of a selection) and fades elsewhere.
    // Only opacity changes, never width, so nothing moves.
    {
        const float faded = 0.25f;
        int a = std::min(cur_, anchor_).line, b = std::max(cur_, anchor_).line;
        if (mark_a_.size() != lines_.size()) {
            mark_a_.assign(lines_.size(), p.fade_syntax ? faded : 1.0f);
            for (int l = a; l <= b && l < (int)mark_a_.size(); l++) mark_a_[l] = 1.0f;
        }
        for (int l = 0; l < (int)mark_a_.size(); l++) {
            float target = !p.fade_syntax || (l >= a && l <= b) ? 1.0f : faded;
            mark_a_[l] = anim::approach(mark_a_[l], target, 22.0f, dt, 0.005f);
        }
    }

    // ---- render
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p.min, p.max, true);
    origin_y_ = p.min.y + p.top_pad - scroll_;
    auto& V = lay_[mode];

    // Switching between edit and reader: the morph draws the page instead.
    bool morphing = false;
    if (morph_.active) {
        float elapsed = anim::enabled ? (float)(anim::clock - morph_.start) * anim::speed : 1e9f;
        if (lay_[1].size() != lines_.size() || !draw_morph(dl, elapsed)) morph_.active = false;
        else { morphing = true; anim::active = true; }
    }
    if (!morphing) {

    // Change pulse: what an undo, redo, paste or replace changed glows softly
    // and fades. Any further edit clears it (its positions would be stale).
    if (pulse_.active && (version_ != pulse_.version || reader_)) pulse_.active = false;
    if (pulse_.active) {
        float t = anim::progress(pulse_.start, anim::clock, 0.4f);
        float a = (1.0f - t) * (1.0f - t);
        if (t >= 1.0f || a <= 0.0f) {
            pulse_.active = false;
        } else {
            ImU32 col = fade(col_alpha(T.accent, 0.16f * a));
            for (auto& s : pulse_.spans) draw_span(dl, s.first, s.second, col);
        }
    }

    // Find: every match softly lit; the current one is the selection.
    find_refresh();
    if (find_active_ && !reader_ && !matches_.empty()) {
        ImU32 col = fade(col_alpha(T.bright, 0.10f));  // other matches: neutral; the current one is the (accent) selection
        int first = std::max(0, line_at_y(0, std::max(0.0f, scroll_ - p.top_pad)) - 1);
        for (int k = 0; k < (int)matches_.size(); k++) {
            if (k == match_) continue;
            if (matches_[k].first.line < first) continue;
            if (origin_y_ + V[std::min(matches_[k].first.line, (int)V.size() - 1)].dy > p.max.y) break;
            draw_span(dl, matches_[k].first, matches_[k].second, col);
        }
    }

    // Selection behind text. Each visual row of the selection is a rect whose
    // edges glide to their targets, like the caret: extending a selection
    // grows it, a new row grows in from the side the caret is moving from,
    // and a cleared selection fades rather than blinking off.
    {
        bool changed = cur_ != sel_cur_ || anchor_ != sel_anchor_;
        bool edited = version_ != sel_version_;
        sel_cur_ = cur_;
        sel_anchor_ = anchor_;
        sel_version_ = version_;
        for (auto& r : sel_rects_) r.live = false;

        if (!reader_ && has_selection()) {
            TextPos a = std::min(cur_, anchor_), b = std::max(cur_, anchor_);
            bool forward = anchor_ < cur_;
            for (int l = a.line; l <= b.line; l++) {
                const Line& L = V[l];
                if (L.img) continue;  // tinted over the image instead (draw_image)
                float ly = origin_y_ + L.dy + L.pad + yoff_[l];
                if (ly > p.max.y || ly + L.h < p.min.y) continue;
                span_rows(l, a, b, [&](int r, float t1, float t2) {
                    auto it = std::find_if(sel_rects_.begin(), sel_rects_.end(),
                                           [&](const SelRect& s) { return s.line == l && s.row == r; });
                    if (it == sel_rects_.end()) {
                        SelRect s{l, r, t1, t2, t1, t2, 1.0f, true};
                        // Rows that appear because the selection grew start
                        // collapsed on the side the caret is coming from.
                        // Rows that merely scroll into view appear as they are.
                        if (changed && !edited) s.x1 = s.x2 = forward ? t1 : t2;
                        sel_rects_.push_back(s);
                    } else {
                        it->t1 = t1;
                        it->t2 = t2;
                        it->a = 1.0f;
                        it->live = true;
                    }
                });
            }
        }
        for (auto& r : sel_rects_) {
            if (r.live) {
                r.x1 = anim::approach(r.x1, r.t1, 50.0f, dt, 0.3f);
                r.x2 = anim::approach(r.x2, r.t2, 50.0f, dt, 0.3f);
            } else {
                // An edit replaced the selected text: drop at once so no
                // stale highlight sits over new text. Otherwise fade out.
                r.a = edited ? 0.0f : anim::approach(r.a, 0.0f, 32.0f, dt, 0.02f);
            }
        }
        sel_rects_.erase(std::remove_if(sel_rects_.begin(), sel_rects_.end(),
                                        [&](const SelRect& s) {
                                            return (!s.live && s.a <= 0.0f) || s.line >= (int)V.size() ||
                                                   s.row >= (int)V[s.line].rows.size();
                                        }),
                         sel_rects_.end());
        if (reader_) sel_rects_.clear();
        for (auto& s : sel_rects_) {
            const Line& L = V[s.line];
            float ry = origin_y_ + L.dy + L.pad + yoff_[s.line] + s.row * L.row_h;
            if (ry > p.max.y || ry + L.row_h < p.min.y || s.x2 - s.x1 < 0.5f) continue;
            dl->AddRectFilled(ImVec2(std::round(col_x_ + s.x1), ry), ImVec2(std::round(col_x_ + s.x2), ry + L.row_h),
                              fade(col_alpha(T.selection, s.a)));
        }
    }

    if (!reader_) draw_tiles_gone(dl);  // under the images closing up over them

    int first = V.empty() ? 0 : line_at_y(mode, std::max(0.0f, scroll_ - p.top_pad - view_h_ * 0.5f));
    for (int i = first; i < (int)V.size(); i++) {
        if (origin_y_ + V[i].dy - view_h_ * 0.5f > p.max.y) break;
        draw_line(dl, mode, i, col_x_, origin_y_);
    }

    // Removed lines: the text fades out, then the space closes. Lines merged
    // into a neighbour have no text to fade and just give up their space.
    if (mode == 0) {
        for (const Ghost& g : ghosts_) {
            float top = origin_y_ + g.dy;
            if (top > p.max.y || top + g.h < p.min.y) continue;
            // Inside a code block the background fills the ghost's space for
            // as long as it's there, so the block never shows a hole.
            if (g.lay.info.kind == LineKind::CodeBlock || g.lay.info.kind == LineKind::Fence) {
                float pad_x = std::round(P_.size * 0.6f);
                float from = top + g.lay.pad, to = top + g.h;
                if (to > from)
                    dl->AddRectFilled(ImVec2(col_x_ - pad_x, from), ImVec2(col_x_ + P_.column + pad_x, to), fade(T.code_bg));
            }
            if (!g.show_text || g.a <= 0.0f) continue;
            float saved = P_.alpha;
            P_.alpha *= g.a;
            dl->PushClipRect(ImVec2(p.min.x, std::max(p.min.y, top)), ImVec2(p.max.x, std::min(p.max.y, top + g.h)), true);
            draw_line_body(dl, 0, g.lay, g.text, g.lay.style, top + g.lay.pad, -1, col_x_, g.full);
            dl->PopClipRect();
            P_.alpha = saved;
        }
    }

    if (!reader_) draw_image_drag(dl);

    // Caret.
    if (!reader_ && !lay_[0].empty()) {
        ImVec2 c = pos_xy(cur_);
        const Line& L = lay_[0][cur_.line];
        float ch = std::round(L.size * 0.92f);
        // On a (shortened) blank line the caret sits where the first typed
        // character will appear, so starting to type never moves it.
        float full_row = L.info.kind == LineKind::Blank ? std::round(P_.size / kEm * P_.line_height) : L.row_h;
        float tx = c.x, ty = c.y + (full_row - ch) * 0.5f + yoff_[cur_.line];
        // Block covers the character after the caret (a space's width at the
        // end of a line); bars sit in the gap before it.
        const std::string& line = lines_[cur_.line];
        Metrics m = metrics(L.size);
        bool block = p.caret == EditorParams::Caret::Block && !L.img;
        float tw = caret_width(p.caret, p.size, p.dpi);
        if (block) {
            if (cur_.col < (int)line.size()) {
                int len;
                unsigned cp = utf8::decode(line.data() + cur_.col, line.data() + line.size(), &len);
                tw = advance(m, cp, L.style[cur_.col]);
            } else {
                tw = m.sans->GetCharAdvance(' ');
            }
        }
        // The caret is a smear rect in document coordinates, so scrolling
        // moves it with the text instead of animating it.
        float w = std::round(tw);
        ImVec2 cmin(tx - (block ? 0.0f : std::round(w * 0.4f)), ty), cmax(cmin.x + w, ty + ch);
        if (caret_snap_ || !caret_.valid() || std::fabs(ty - caret_.current_min().y) > view_h_ * 2) {
            caret_.snap(cmin, cmax);
            caret_snap_ = false;
        }
        caret_.set(cmin, cmax, p.caret_trail, 50.0f);
        caret_.update(dt);
        // Carrying an image, the caret steps back so the drop marker is the
        // only accent mark saying "here".
        bool carrying = img_drag_ || (land_line_ >= 0 && land_drag_);
        caret_shown_ = anim::approach(caret_shown_, carrying ? 0.0f : 1.0f, 24.0f, dt, 0.01f);
        bool on = (!p.blink || !p.keyboard || std::fmod(now - activity_, 1.1) < 0.6) && caret_shown_ > 0.0f;
        // On an image line there's no caret (an image is picked, or a
        // selection ends there); leaving, it appears where it lands.
        if (L.img) {
            on = false;
            caret_snap_ = true;
        }
        if (on) {
            ImU32 col = col_alpha(p.keyboard ? T.accent : col_alpha(T.accent, 0.4f), caret_shown_);
            caret_.draw(dl, ImVec2(col_x_, origin_y_), fade(col), block ? std::round(p.dpi) : 0.0f);

            // Knock the covered character out of the block once it has
            // settled, so it stays readable.
            bool settled = caret_.settled();
            if (block && settled && p.keyboard && cur_.col < (int)line.size() && line[cur_.col] != '\t') {
                Style f = L.style[cur_.col];
                bool mono = is_mono(f), bold = bold_of(f);
                ImFont* bold_font = mono ? p.mono_bold : p.sans_bold;
                ImFont* font = bold && bold_font ? bold_font : (mono ? p.mono : p.sans);
                float size = mono ? m.msize : m.size;
                float row_top = origin_y_ + c.y + yoff_[cur_.line];
                float text_y = row_top + (L.row_h - L.size) * 0.5f + (mono ? m.mono_dy : 0.0f);
                int end = utf8::next(line, cur_.col);
                dl->AddText(font, size, ImVec2(std::round(col_x_ + tx), std::round(text_y)), fade(T.bg),
                            line.data() + cur_.col, line.data() + end);
            }
        }
    }
    draw_completion(dl, dt);

    }  // !morphing

    // Scroll position hint: a thin bar at the right edge that shows while
    // scrolling and fades out after.
    if (max_scroll > 1) {
        float t = (float)(now - scroll_activity_);
        float a = t < 0.8f ? 1.0f : std::max(0.0f, 1.0f - (t - 0.8f) / 0.35f);
        if (a > 0) {
            if (a < 1) anim::active = true;
            float track = view_h_ - 16 * p.dpi;
            float th = std::max(24 * p.dpi, track * view_h_ / content_h);
            float ty = p.min.y + 8 * p.dpi + (track - th) * (scroll_ / max_scroll);
            float x = p.max.x - 7 * p.dpi;
            dl->AddRectFilled(ImVec2(x, ty), ImVec2(x + 3 * p.dpi, ty + th), fade(col_alpha(T.faint, a)), 2 * p.dpi);
        }
    }
    dl->PopClipRect();
}

// Fill the visual rows covering text a..b (edit view), as the selection does.
void Editor::draw_span(ImDrawList* dl, TextPos a, TextPos b, ImU32 col) {
    auto& V = lay_[0];
    for (int l = std::max(0, a.line); l <= b.line && l < (int)V.size(); l++) {
        const Line& L = V[l];
        if (L.img) continue;
        float ly = origin_y_ + L.dy + L.pad + yoff_[l];
        if (ly > P_.max.y || ly + L.dh < P_.min.y) continue;
        span_rows(l, a, b, [&](int r, float x1, float x2) {
            float ry = ly + r * L.row_h;
            dl->AddRectFilled(ImVec2(std::round(col_x_ + x1), ry), ImVec2(std::round(col_x_ + x2), ry + L.row_h), col);
        });
    }
}

// The visual rows of text line l (edit view) that a..b covers: f(row, x1,
// x2), x from the column's left. A row the span runs on past the end of
// takes a space more, for the line break.
template <class F>
void Editor::span_rows(int l, TextPos a, TextPos b, F&& f) const {
    const Line& L = lay_[0][l];
    const std::string& s = lines_[l];
    Metrics m = metrics(L.size);
    int la = l == a.line ? a.col : 0;
    int lb = l == b.line ? b.col : (int)s.size();
    for (int r = 0; r < (int)L.rows.size(); r++) {
        int ra = L.rows[r];
        int rb = r + 1 < (int)L.rows.size() ? L.rows[r + 1] : (int)s.size();
        int sa = std::max(la, ra), sb = std::min(lb, rb);
        bool eol = l != b.line && r + 1 == (int)L.rows.size();
        if (sa > sb || (sa == sb && !eol)) continue;
        float x1 = row_x(L, r) + measure(s, L.style, ra, sa, m);
        float x2 = row_x(L, r) + measure(s, L.style, ra, sb, m);
        if (eol) x2 += m.sans->GetCharAdvance(' ');
        f(r, x1, x2);
    }
}

// The edit <-> reader transition. Every visible word exists in both views
// (the reader text maps back to source bytes), so it's drawn at a position
// eased between the two. Syntax that only the editor shows fades out where
// it stands; decorations only the reader has (bullet, checkbox, quote bar)
// fade in where they'll be. Each view keeps its own scroll, so lines ease
// between their two screen positions. At t = 1 this draws exactly what the
// normal reader (or editor) frame does, so the hand-off is invisible.
bool Editor::draw_morph(ImDrawList* dl, float s) {
    const EditorParams& p = P_;
    // Every part is its own exponential (Unreal's FInterpTo shape: fast out
    // of the gate, soft settle), none cut short. s is seconds since the
    // toggle. What's leaving (syntax, words that re-wrap onto another row)
    // fades out fast; words that keep their row glide; what's arriving fades
    // in once the glide is most of the way there, so nothing crosses.
    auto expo = [](float x, float rate) { return x <= 0 ? 0.0f : 1.0f - std::exp(-rate * x); };
    const float glide_delay = 0.02f, glide_rate = 38.0f;
    float fade_out = 1.0f - expo(s, 70.0f);                       // ~95% gone by 43ms
    float g = expo(s - glide_delay, glide_rate);                  // glide progress
    float in_start = glide_delay + std::log(33.0f) / glide_rate;  // glide at 97%
    float fade_in = expo(s - in_start, 45.0f);
    // Symbols with a counterpart in the other view (- and a bullet, [ ] and
    // a checkbox, > and the quote bar, --- and the rule) sit in the same
    // spot, so they transform in place: one expo crossfade, old out as new
    // comes in, no gap between them.
    float swap = expo(s, 34.0f);
    float struct_old = 1.0f - swap, struct_new = swap;  // "old" = the view being left
    if (g >= 0.995f && fade_in >= 0.995f) return false;          // settled: hand back
    float w = morph_.to_reader ? g : 1.0f - g;
    float wg = w;                                                 // glide position

    // Where the normal renderer puts byte k on row r: run starts (a run is
    // same-style text) snap to whole pixels, glyphs inside sit at fractional
    // offsets. Matching that exactly means a word that doesn't move sits on
    // the same pixel before, during and after the morph.
    auto glyph_x = [&](const Line& L, const std::string& s, const std::vector<Style>& st, const Metrics& m, int r, int k) {
        int rs = L.rows[r];
        int s0 = k;
        while (s0 > rs && st[s0 - 1] == st[k] && (s[s0 - 1] == '\t') == (s[k] == '\t')) s0--;
        return std::round(col_x_ + row_x(L, r) + measure(s, st, rs, s0, m)) + measure(s, st, s0, k, m);
    };
    exact_x_ = true;
    // ImGui snaps the start of every text draw to a whole pixel. Inside a run
    // a word sits at a fractional position, so drawn on its own it would jump.
    // The positions above are already exact: tell ImGui to leave them alone.
    const ImDrawListFlags saved_flags = dl->Flags;
    dl->Flags |= ImDrawListFlags_TextNoPixelSnap;
    // Syntax only the editor shows (**, #, backticks, urls) takes turns with
    // the glide: going to the reader it leaves first, coming back it arrives
    // last, so words never glide across it. Structural symbols crossfade in
    // place with their counterpart instead (see swap above).
    float syntax_a = morph_.to_reader ? fade_out : fade_in;
    float edit_struct = morph_.to_reader ? struct_old : struct_new;    // -, [ ], >, ---
    float reader_struct = morph_.to_reader ? struct_new : struct_old;  // bullet, box, bar, rule
    auto lerp = [](float a, float b, float k) { return a + (b - a) * k; };
    const float base = P_.alpha;
    float scroll_e = morph_.to_reader ? morph_.scroll_from : scroll_;
    float scroll_r = morph_.to_reader ? scroll_ : morph_.scroll_from;
    float oe = p.min.y + p.top_pad - scroll_e, orr = p.min.y + p.top_pad - scroll_r;
    const float x0 = col_x_;
    const float pad_x = std::round(p.size * 0.6f);
    auto& E = lay_[0];
    auto& R = lay_[1];
    dl->PushClipRect(p.min, p.max, true);

    for (int i = 0; i < (int)lines_.size(); i++) {
        const Line& L0 = E[i];
        const Line& L1 = R[i];
        float ye = oe + L0.dy + L0.pad, yr = orr + L1.y + L1.pad;  // first-row tops
        float he = L0.rows.size() * L0.row_h, hr = L1.rows.size() * L1.row_h;
        if (std::min(ye, yr) > p.max.y || std::max(ye + he, yr + hr) < p.min.y) continue;
        const std::string& src = lines_[i];
        const std::string& rt = L1.rtext;
        Metrics m0 = metrics(L0.size), m1 = metrics(L1.size);

        // Images are the same in both views: they just glide with their line.
        if (L0.img && L1.img) {
            draw_image(dl, L0, x0, lerp(ye, yr, wg), base, -1);
            continue;
        }

        // Code blocks reshape between the two heights (fences are thinner
        // in the reader), staying solid throughout.
        if (L0.info.kind == LineKind::CodeBlock || L0.info.kind == LineKind::Fence) {
            float top = lerp(ye, yr, w), h = lerp(he, hr, w);
            dl->AddRectFilled(ImVec2(x0 - pad_x, top), ImVec2(x0 + p.column + pad_x, top + h), col_alpha(T.code_bg, base));
        }
        // Rules: the editor's short line after "---" gives way to the
        // reader's full-width one.
        if (L0.info.kind == LineKind::Rule) {
            float th = std::max(1.0f, p.dpi);
            float me = std::round(ye + L0.row_h * 0.5f), mr = std::round(yr + L1.row_h * 0.5f);
            float from = x0 + measure(src, L0.style, 0, (int)src.size(), m0) + L0.size * 0.6f;
            dl->AddRectFilled(ImVec2(from, me), ImVec2(x0 + p.column, me + th), col_alpha(T.border, base * edit_struct));
            dl->AddRectFilled(ImVec2(x0, mr), ImVec2(x0 + p.column, mr + th), col_alpha(T.border, base * reader_struct));
        }

        // Words: each run of reader text glides from its source position in
        // the editor to its reader position. Runs break at spaces, row ends
        // and wherever the mapping to source jumps (removed syntax).
        std::vector<char> covered(src.size(), 0);
        auto is_deco = [&](Style f) {
            return (f & (S_TaskBox | S_QuoteMark)) || ((f & S_ListMark) && L0.info.list && !L0.info.ordered);
        };
        for (int k = 0; k < (int)rt.size();) {
            Style f = L1.rstyle[k];
            int r1 = row_of(L1, k);
            int r1_end = r1 + 1 < (int)L1.rows.size() ? L1.rows[r1 + 1] : (int)rt.size();
            bool deco = is_deco(f);
            // A run is one word plus the space after it, in one style, on one
            // row, mapping to consecutive source bytes.
            int k2 = k;
            while (true) {
                bool was_space = rt[k2] == ' ';
                k2 = utf8::next(rt, k2);
                if (k2 >= r1_end || L1.rstyle[k2] != f) break;
                if (deco) continue;
                if (was_space || L1.rmap[k2] != L1.rmap[k2 - 1] + 1) break;
            }
            if (!deco) {
                int j = L1.rmap[k];
                int r0 = row_of(L0, j);
                int r0_end = r0 + 1 < (int)L0.rows.size() ? L0.rows[r0 + 1] : (int)src.size();
                if (j + (k2 - k) > r0_end && r0_end > j) k2 = k + (r0_end - j);  // stay on one editor row
            }
            float xr = glyph_x(L1, rt, L1.rstyle, m1, r1, k);
            float yr_row = yr + r1 * L1.row_h;
            if (deco) {
                P_.alpha = base * reader_struct;  // bullets, boxes and bars all have an editor counterpart
                if (P_.alpha > 0) draw_row(dl, 1, L1, m1, rt, L1.rstyle, k, k2, xr, yr_row);
            } else {
                int j = L1.rmap[k];
                int r0 = row_of(L0, j);
                float xe = glyph_x(L0, src, L0.style, m0, r0, j);
                float ye_row = ye + r0 * L0.row_h;
                for (int b = j; b < j + (k2 - k) && b < (int)src.size(); b++) covered[b] = 1;
                if (r0 == r1) {
                    // Same row in both views: glide, closing the gaps the
                    // syntax leaves (or opening them again).
                    P_.alpha = base;
                    draw_row(dl, 1, L1, m1, rt, L1.rstyle, k, k2, lerp(xe, xr, wg), lerp(ye_row, yr_row, wg));
                } else {
                    // Re-wrapped onto another row: travelling would cross other
                    // words, so it fades out where it was, then in where it
                    // belongs once that spot has settled.
                    float fx = morph_.to_reader ? xe : xr, fy = morph_.to_reader ? ye_row : yr_row;
                    float tx = morph_.to_reader ? xr : xe, ty = morph_.to_reader ? yr_row : ye_row;
                    if (fade_out > 0) { P_.alpha = base * fade_out; draw_row(dl, 1, L1, m1, rt, L1.rstyle, k, k2, fx, fy); }
                    if (fade_in > 0) { P_.alpha = base * fade_in; draw_row(dl, 1, L1, m1, rt, L1.rstyle, k, k2, tx, ty); }
                }
            }
            k = k2;
        }

        // Syntax only the editor shows: fades out where it stands (at the
        // dimness fade_syntax gave it).
        line_mark_a_ = syntax_ == Syntax::Markdown && i < (int)mark_a_.size() ? mark_a_[i] : 1.0f;
        for (int j = 0; j < (int)src.size();) {
            if (covered[j]) { j++; continue; }
            int r0 = row_of(L0, j);
            int r0_end = r0 + 1 < (int)L0.rows.size() ? L0.rows[r0 + 1] : (int)src.size();
            // One style per call, so each run is placed exactly (see glyph_x).
            int j2 = j;
            while (j2 < r0_end && !covered[j2] && L0.style[j2] == L0.style[j]) j2 = utf8::next(src, j2);
            float xe = glyph_x(L0, src, L0.style, m0, r0, j);
            bool structural = (L0.style[j] & (S_ListMark | S_TaskBox | S_QuoteMark)) || L0.info.kind == LineKind::Rule;
            P_.alpha = base * (structural ? edit_struct : syntax_a);
            if (P_.alpha > 0) draw_row(dl, 0, L0, m0, src, L0.style, j, j2, xe, ye + r0 * L0.row_h);
            j = j2;
        }

        // Line numbers belong to the editor.
        if (p.line_numbers && x0 - p.min.x > p.size * 4 && syntax_a > 0) {
            float ns = std::round(p.size * p.small_size);
            char buf[16];
            snprintf(buf, sizeof buf, "%d", i + 1);
            ImVec2 sz = p.mono->CalcTextSizeA(ns, FLT_MAX, 0, buf);
            float ty = ye + (L0.row_h - L0.size) * 0.5f + (L0.size - ns) * 0.78f;
            dl->AddText(p.mono, ns, ImVec2(std::round(x0 - p.size * 1.3f - sz.x), std::round(ty)),
                        col_alpha(i == cur_.line ? T.accent : T.dim, base * syntax_a), buf);
        }
    }
    P_.alpha = base;
    exact_x_ = false;
    dl->Flags = saved_flags;
    dl->PopClipRect();
    return true;
}

void Editor::draw_line(ImDrawList* dl, int mode, int i, float x0, float y0) {
    const Line& L = lay_[mode][i];
    float top = y0 + L.dy + yoff_[i];
    // While a line's space is still opening (new, or grown a wrapped row),
    // rows that don't have their room yet stay hidden and fade in once they
    // do. Drawn whole, never clipped.
    draw_line_body(dl, mode, L, text_of(mode, i), style_of(mode, i), top + L.pad, i, x0, mode == 0 ? L.dh : L.h);
}

// Draws a laid-out line with its first row's top at y. `index` is its line
// number, or -1 for a ghost (no line number or problems). `shown_h` is how
// much of its height has opened: a row fades in only once its room is there.
void Editor::draw_line_body(ImDrawList* dl, int mode, const Line& L, const std::string& s,
                            const std::vector<Style>& st, float y, int index, float x0, float shown_h) {
    const int i = index;
    line_mark_a_ = i < 0 ? 0.25f
                 : mode == 0 && syntax_ == Syntax::Markdown && i < (int)mark_a_.size() ? mark_a_[i] : 1.0f;
    Metrics m = metrics(L.size);

    // Space first, then text: a row stays hidden until most of its room has
    // opened, then fades in over the rest.
    struct Restore { float& a; float v; ~Restore() { a = v; } } restore{P_.alpha, P_.alpha};
    const float base = P_.alpha;
    auto row_alpha = [&](int r) {
        float cover = (shown_h - L.pad - r * L.row_h) / std::max(1.0f, L.row_h);
        if (cover >= 1.0f) return 1.0f;
        return anim::ease_out(std::clamp((cover - 0.6f) / 0.4f, 0.0f, 1.0f));
    };
    float pad_x = std::round(P_.size * 0.6f);
    float block_h = L.rows.size() * L.row_h;

    // A code block's background belongs to the space, not the text: it
    // covers exactly the height that's open and doesn't fade, so the block
    // stays whole while lines inside it open or close. (Ghosts draw their
    // own, since their space outlives their text.)
    if (index >= 0 && (L.info.kind == LineKind::CodeBlock || L.info.kind == LineKind::Fence)) {
        float open = std::clamp(shown_h - L.pad, 0.0f, block_h);
        if (open > 0.0f)
            dl->AddRectFilled(ImVec2(x0 - pad_x, y), ImVec2(x0 + P_.column + pad_x, y + open), col_alpha(T.code_bg, base));
    }

    P_.alpha = base * row_alpha(0);
    if (P_.alpha <= 0.0f && shown_h < L.h) return;

    if (L.info.kind == LineKind::Rule) {
        float mid = std::round(y + L.row_h * 0.5f);
        float from = mode ? x0 : x0 + measure(s, st, 0, (int)s.size(), m) + L.size * 0.6f;
        dl->AddRectFilled(ImVec2(from, mid), ImVec2(x0 + P_.column, mid + std::max(1.0f, P_.dpi)), fade(T.border));
    }

    // Line number in the left margin, only where there's room.
    if (i >= 0 && mode == 0 && P_.line_numbers && x0 - P_.min.x > P_.size * 4) {
        float ns = std::round(P_.size * P_.small_size);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", i + 1);
        ImVec2 sz = P_.mono->CalcTextSizeA(ns, FLT_MAX, 0, buf);
        float ty = y + (L.row_h - L.size) * 0.5f + (L.size - ns) * 0.78f;
        if (L.img) ty = y + L.img_m;  // level with the image's top
        ImU32 col = i == cur_.line ? T.accent : T.dim;  // where you are
        dl->AddText(P_.mono, ns, ImVec2(std::round(x0 - P_.size * 1.3f - sz.x), std::round(ty)), fade(col), buf);
    }

    if (L.img) {
        draw_image(dl, L, x0, y, P_.alpha, mode == 0 ? i : -1);
        return;
    }

    for (int r = 0; r < (int)L.rows.size(); r++) {
        int a = L.rows[r];
        int b = r + 1 < (int)L.rows.size() ? L.rows[r + 1] : (int)s.size();
        P_.alpha = base * row_alpha(r);
        if (P_.alpha <= 0.0f) continue;
        draw_row(dl, mode, L, m, s, st, a, b, x0 + row_x(L, r), y + r * L.row_h);
    }
    P_.alpha = base * row_alpha(0);

    // Problems on this line: the part each is about underlined, and the
    // first error's message after the row it's on, where the eye already is.
    // The message crossfades with the line's hint (after the last row).
    if (mode == 0 && i >= 0) {
        const Mark* mk = nullptr;
        for (auto& k : marks_)
            if (k.d.line == i && k.d.kind == Diagnostic::Kind::Error && (!mk || k.a > mk->a)) mk = &k;
        float pa = mk ? mk->a : 0.0f;
        auto hint = hints_.find(i);
        // An image line whose image can't be shown says so, quietly.
        std::string hint_text = hint != hints_.end() ? hint->second : std::string();
        if (hint_text.empty() && L.info.kind == LineKind::Image && !L.img) {
            std::string src;
            md_image_line(lines_[i], nullptr, &src);
            hint_text = src.find("://") != std::string::npos ? "web images aren't shown" : "image not found";
        }

        int rows = (int)L.rows.size();
        float ms = std::round(P_.size * P_.ui_size);
        float clip_x = P_.max.x - P_.size;
        auto note_x = [&](int r) {
            int ra = L.rows[r], rb = r + 1 < rows ? L.rows[r + 1] : (int)s.size();
            return x0 + row_x(L, r) + measure(s, st, ra, rb, m) + P_.size * 1.2f;
        };
        auto note = [&](int r, const std::string& text, ImU32 col) {
            float ex = note_x(r), ry = y + r * L.row_h;
            float ty = std::round(ry + (L.row_h - ms) * 0.5f + (m.size - ms) * 0.3f);
            dl->PushClipRect(ImVec2(ex, ry), ImVec2(clip_x, ry + L.row_h), true);
            dl->AddText(P_.mono, ms, ImVec2(std::round(ex), ty), col, text.c_str());
            dl->PopClipRect();
        };
        // The error's own row when the message fits beside it; the last row
        // (which usually has room) when it doesn't.
        int err_row = rows - 1;
        if (mk && mk->d.col0 >= 0) {
            int r = row_of(L, std::clamp(mk->d.col0, 0, (int)s.size()));
            if (note_x(r) + P_.mono->CalcTextSizeA(ms, FLT_MAX, 0, mk->d.msg.c_str()).x <= clip_x) err_row = r;
        }

        if (!hint_text.empty() && pa < 1.0f)
            note(rows - 1, hint_text, col_alpha(fade(T.dim), 1.0f - pa));
        // The underlines, row by row if a range wraps. No range: the whole
        // line. They sit where an underline does, just under the baseline;
        // on a link, which has its own, just under that. Spelling gets dots,
        // quieter than an error's solid line.
        float uh = std::max(1.0f, std::round(P_.dpi));
        for (auto& k : marks_) {
            if (k.d.line != i || k.a <= 0.0f) continue;
            bool spell = k.d.kind == Diagnostic::Kind::Spelling;
            ImU32 col = col_alpha(fade(spell ? T.spelling : T.error), k.a);
            int c0 = k.d.col0 >= 0 ? std::clamp(k.d.col0, 0, (int)s.size()) : 0;
            int c1 = k.d.col0 >= 0 ? std::clamp(k.d.col1, 0, (int)s.size()) : (int)s.size();
            if (c1 <= c0) continue;
            for (int r = 0; r < (int)L.rows.size(); r++) {
                int ra = L.rows[r], rb = r + 1 < (int)L.rows.size() ? L.rows[r + 1] : (int)s.size();
                int a = std::max(c0, ra), b = std::min(c1, rb);
                if (a >= b) continue;
                float ux = std::round(x0 + row_x(L, r) + measure(s, st, ra, a, m));
                float ux1 = std::round(ux + measure(s, st, a, b, m));
                bool on_link = false;
                for (int k2 = a; k2 < b && !on_link; k2++) on_link = (st[k2] & (S_Link | S_WikiLink)) != 0;
                float text_top = y + r * L.row_h + (L.row_h - L.size) * 0.5f;
                float uy = std::round(text_top + L.size * 0.86f) + (on_link ? std::max(2 * uh, std::round(L.size * 0.14f)) : 0.0f);
                if (!spell) {
                    dl->AddRectFilled(ImVec2(ux, uy), ImVec2(ux1, uy + uh), col);
                } else {
                    float dot = std::max(1.0f, std::round(uh * 1.5f));
                    for (float dx = ux; dx < ux1; dx += dot * 2) dl->AddRectFilled(ImVec2(dx, uy), ImVec2(std::min(dx + dot, ux1), uy + dot), col);
                }
            }
        }
        if (pa > 0.0f) note(err_row, mk->d.msg, col_alpha(fade(col_mix(T.dim, T.text, 0.45f)), pa));
    }
}

namespace {

struct Look {
    ImU32 col;
    bool italic = false, strike = false, underline = false;
};

Look resolve(Style f, const LineInfo& info, bool reader) {
    Look k{T.text};
    if (f & S_Heading) k.col = T.bright;
    if (f & S_Quote) { k.col = col_mix(T.text, T.dim, 0.35f); k.italic = true; }
    if (f & S_Bold) k.col = T.bright;
    if (f & S_Italic) k.italic = true;
    if (f & S_Strike) { k.strike = true; k.col = T.dim; }
    if (f & S_Highlight) k.col = T.bright;
    // Structural markers (bullets, >, [ ]) sit one step above other syntax.
    ImU32 structural = col_mix(T.dim, T.text, 0.45f);
    if (f & (S_Link | S_WikiLink)) { k.col = T.bright; k.underline = true; }
    // A tag is one token, # and all: set in mono (see is_mono), a step
    // quieter than the text around it (it says what the note is, not what
    // it says).
    if (f & S_Tag) { k.col = col_mix(T.text, T.dim, 0.4f); k.italic = false; }
    if (f & (S_Code | S_CodeBlock)) { k.col = T.code; k.italic = false; }
    if (f & S_TaskDone) { k.col = T.dim; k.strike = !(f & S_Marker); }
    if (f & S_ListMark) k.col = structural;
    if (f & S_Marker) {
        k.col = T.dim;
        k.underline = k.strike = k.italic = false;
        if (f & (S_ListMark | S_QuoteMark | S_TaskBox)) k.col = structural;
        if ((f & S_TaskBox) && info.done) k.col = T.dim;
    }
    if (reader && (f & S_ListMark)) k.col = T.dim;
    return k;
}

// delta.ini: sections stand out, values read brighter than their keys,
// syntax and comments step back.
Look resolve_ini(Style f) {
    Look k{T.text};
    if (f & I_Comment) k.col = T.dim;
    else if (f & I_Section) k.col = T.bright;
    else if (f & (I_Bracket | I_Eq)) k.col = T.dim;
    else if (f & I_Value) k.col = T.bright;
    else if (f & I_Key) k.col = col_mix(T.text, T.dim, 0.2f);
    return k;
}

void put_text(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 col, const char* a, const char* b,
              bool bold, bool italic, bool exact_x = false) {
    int v0 = dl->VtxBuffer.Size;
    // Run starts snap to whole pixels; the morph passes exact positions it
    // has already computed to match that snapping (and to glide smoothly).
    ImVec2 pos(exact_x ? x : std::round(x), std::round(y));
    dl->AddText(font, size, pos, col, a, b);
    if (bold) dl->AddText(font, size, ImVec2(pos.x + std::max(0.5f, size * 0.035f), pos.y), col, a, b);
    if (italic) {
        float mid = pos.y + size * 0.6f;
        for (int k = v0; k < dl->VtxBuffer.Size; k++) dl->VtxBuffer[k].pos.x += (mid - dl->VtxBuffer[k].pos.y) * 0.19f;
    }
}

}  // namespace

void Editor::draw_row(ImDrawList* dl, int mode, const Line& L, const Metrics& m, const std::string& s,
                      const std::vector<Style>& st, int a, int b, float x0, float ry) {
    const char* p = s.data();
    float ty = ry + (L.row_h - L.size) * 0.5f;
    float mono_ty = ty + m.mono_dy;
    float dpi = P_.dpi;

    // Pass 1: inline code / highlight pills.
    {
        float x = x0;
        int i = a;
        while (i < b) {
            Style f = st[i];
            int key = (f & S_Code) ? 1 : (f & S_Highlight) ? 2 : 0;
            float w = 0;
            int j = i;
            while (j < b) {
                Style g = st[j];
                int k2 = (g & S_Code) ? 1 : (g & S_Highlight) ? 2 : 0;
                if (k2 != key) break;
                int len;
                unsigned cp = utf8::decode(p + j, p + s.size(), &len);
                w += advance(m, cp, g);
                j += len;
            }
            if (key) {
                float h = std::round(L.size * 0.95f);
                float top = std::round(ry + (L.row_h - h) * 0.5f);
                ImU32 c = key == 1 ? col_mix(T.code_bg, T.border, 0.5f) : T.highlight;
                dl->AddRectFilled(ImVec2(x - 2 * dpi, top), ImVec2(x + w + 2 * dpi, top + h), fade(c), 2 * dpi);
            }
            x += w;
            i = j;
        }
    }

    // Pass 2: text in runs of identical style.
    float x = x0;
    int i = a;
    while (i < b) {
        Style f = st[i];
        int j = i;
        float w = 0;
        bool tab = s[i] == '\t';
        while (j < b && st[j] == f && (s[j] == '\t') == tab) {
            int len;
            unsigned cp = utf8::decode(p + j, p + s.size(), &len);
            w += advance(m, cp, f);
            j += len;
        }
        Look k = syntax_ == Syntax::Ini ? resolve_ini(f) : resolve(f, L.info, mode == 1);
        ImU32 col = fade(k.col);
        // Structural markers (bullets, >, [ ]) are part of reading a list, and
        // a link's address is content, so only the other syntax fades away
        // from the caret.
        if ((f & S_Marker) && !(f & (S_ListMark | S_QuoteMark | S_TaskBox | S_Url)))
            col = fade(col_alpha(k.col, line_mark_a_));
        bool mono = is_mono(f);

        // Colour values get a swatch just after them.
        if (syntax_ == Syntax::Ini && (f & I_Color) && j - i == 7 && (j >= b || !(st[j] & I_Color))) {
            unsigned long rgb = std::strtoul(s.substr(i + 1, 6).c_str(), nullptr, 16);
            ImU32 sw = IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
            float sz = std::round(m.msize * 0.62f);
            float sx = std::round(x + w + m.msize * 0.5f), sy = std::round(ry + (L.row_h - sz) * 0.5f);
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sz, sy + sz), fade(sw), 2 * dpi);
            dl->AddRect(ImVec2(sx, sy), ImVec2(sx + sz, sy + sz), fade(T.border), 2 * dpi, std::max(1.0f, dpi));
        }

        if (mode == 1 && (f & S_TaskBox)) {
            float bs = std::round(L.size * 0.78f);
            float bx = std::round(x + L.size * 0.05f), by = std::round(ry + (L.row_h - bs) * 0.5f);
            if (f & S_TaskDone) {
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bs, by + bs), fade(T.dim), 3 * dpi);
                ImVec2 c1(bx + bs * 0.24f, by + bs * 0.52f), c2(bx + bs * 0.43f, by + bs * 0.72f), c3(bx + bs * 0.78f, by + bs * 0.3f);
                dl->AddLine(c1, c2, fade(T.bg), 1.8f * dpi);
                dl->AddLine(c2, c3, fade(T.bg), 1.8f * dpi);
            } else {
                dl->AddRect(ImVec2(bx, by), ImVec2(bx + bs, by + bs), fade(T.dim), 3 * dpi, 1.5f * dpi);
            }
        } else if (mode == 1 && (f & S_QuoteMark)) {
            float bw = std::max(2.0f, std::round(3 * dpi));
            dl->AddRectFilled(ImVec2(x, ry), ImVec2(x + bw, ry + L.row_h), fade(T.faint));
        } else if (!tab) {
            // Real SemiBold when we have it; the double-draw only as a fallback.
            bool bold = bold_of(f);
            ImFont* bold_font = mono ? P_.mono_bold : P_.sans_bold;
            ImFont* font = bold && bold_font ? bold_font : (mono ? P_.mono : P_.sans);
            put_text(dl, font, mono ? m.msize : m.size, x, mono ? mono_ty : ty, col, p + i, p + j,
                     bold && !bold_font, k.italic, exact_x_);
            float lw = std::max(1.0f, std::round(dpi));
            if (k.underline) {
                float uy = std::round(ty + L.size * 0.86f);
                dl->AddRectFilled(ImVec2(x, uy), ImVec2(x + w, uy + lw), fade(col_alpha(k.col, 0.45f)));
            }
            if (k.strike) {
                float sy = std::round(ty + L.size * 0.55f);
                dl->AddRectFilled(ImVec2(x, sy), ImVec2(x + w, sy + lw), fade(col_alpha(k.col, 0.8f)));
            }
        }
        x += w;
        i = j;
    }
}
