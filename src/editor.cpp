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

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out(1);
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

bool is_mono(uint16_t f) { return f & (S_Code | S_CodeBlock); }

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
    scroll_ = scroll_target_ = 0;
    snap_ = caret_snap_ = true;
    sync_line_ = -1;
}

void Editor::reload_text(const std::string& text) {
    begin_edit(Edit::Other);
    lines_ = split_lines(text);
    clamp(cur_);
    clamp(anchor_);
    end_edit(false);
}

std::string Editor::text() const {
    std::string out;
    size_t total = 0;
    for (auto& l : lines_) total += l.size() + 1;
    out.reserve(total);
    for (size_t i = 0; i < lines_.size(); i++) {
        if (i) out += '\n';
        out += lines_[i];
    }
    return out;
}

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
    int n = 0;
    for (unsigned char c : selected_text())
        if (!utf8::is_cont(c)) n++;
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
    std::vector<uint16_t> st;
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
        bool in_word = false, has_alnum = false;
        for (size_t i = 0; i <= l.size(); i++) {
            unsigned char c = i < l.size() ? (unsigned char)l[i] : ' ';
            if (c == ' ' || c == '\t') {
                if (in_word && has_alnum) n++;
                in_word = has_alnum = false;
            } else {
                in_word = true;
                if (std::isalnum(c) || c >= 0x80) has_alnum = true;
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

void Editor::begin_edit(Edit kind) {
    double now = ImGui::GetTime();
    bool merge = kind != Edit::Other && kind == last_edit_ && now - last_edit_time_ < 1.2 &&
                 cur_ == last_edit_pos_ && !has_selection();
    if (!merge) {
        undo_.push_back({lines_, cur_, anchor_});
        if (undo_.size() > 500) undo_.erase(undo_.begin());
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

void Editor::undo() {
    if (undo_.empty()) return;
    redo_.push_back({lines_, cur_, anchor_});
    Snap s = std::move(undo_.back());
    undo_.pop_back();
    lines_ = std::move(s.lines);
    cur_ = s.cur;
    anchor_ = s.anchor;
    last_edit_ = Edit::None;
    end_edit();
}

void Editor::redo() {
    if (redo_.empty()) return;
    undo_.push_back({lines_, cur_, anchor_});
    Snap s = std::move(redo_.back());
    redo_.pop_back();
    lines_ = std::move(s.lines);
    cur_ = s.cur;
    anchor_ = s.anchor;
    last_edit_ = Edit::None;
    end_edit();
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
        cur_ = anchor_ = {std::min(l, (int)lines_.size() - 1), cur_.col};
        clamp(cur_);
        anchor_ = cur_;
    }
    end_edit();
}

void Editor::paste() {
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) return;
    std::string text = clip;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    begin_edit(Edit::Other);
    if (!has_selection() && !line_clip_.empty() && text == line_clip_) {
        // A whole-line copy pastes as a line above, like an IDE.
        int col = cur_.col;
        TextPos end = insert({cur_.line, 0}, text);
        cur_ = anchor_ = {end.line, col};
    } else {
        replace_selection(text);
    }
    end_edit();
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
    auto [a, b] = selected_lines();
    int n = (int)lines_.size();
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= n)) return;

    auto& V = lay_[0];
    bool have = (int)V.size() == n && (int)yoff_.size() == n;
    float block_h = 0, other_h = 0;
    if (have) {
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
    end_edit();
}

void Editor::toggle_task_line(int l) {
    LineInfo info;
    std::vector<uint16_t> st;
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

void Editor::restore_view(TextPos cur, float scroll) {
    clamp(cur);
    cur_ = anchor_ = cur;
    scroll_ = scroll_target_ = scroll;
    caret_snap_ = true;
}

void Editor::set_syntax(Syntax s) {
    if (s == syntax_) return;
    syntax_ = s;
    lay_[0].clear();  // styles are cached per line
    lay_[1].clear();
    diag_.clear();
}

void Editor::set_reader(bool r) {
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
    m.msize = std::round(size * 0.9f);
    m.sans = P_.sans->GetFontBaked(size);
    m.mono = P_.mono->GetFontBaked(m.msize);
    m.sans_b = P_.sans_bold ? P_.sans_bold->GetFontBaked(size) : nullptr;
    m.mono_b = P_.mono_bold ? P_.mono_bold->GetFontBaked(m.msize) : nullptr;
    m.tabw = m.sans->GetCharAdvance(' ') * P_.tab_size;
    return m;
}

// The one place that decides which bytes are set in the bold weight, so
// measuring and drawing always agree. Syntax characters stay regular.
bool Editor::bold_of(uint16_t f) const {
    if (syntax_ == Syntax::Ini) return f & I_Section;
    return (f & (S_Bold | S_Heading)) && !(f & (S_Marker | S_Code | S_CodeBlock));
}

float Editor::advance(const Metrics& m, unsigned cp, uint16_t f) const {
    if (cp == '\t') return m.tabw;
    if ((f & S_TaskBox) && !(f & S_Marker)) return std::round(m.sans->Size * 1.3f);  // reader checkbox
    bool b = bold_of(f);
    ImFontBaked* font = is_mono(f) ? (b && m.mono_b ? m.mono_b : m.mono) : (b && m.sans_b ? m.sans_b : m.sans);
    return font->GetCharAdvance((ImWchar)cp);
}

float Editor::measure(const std::string& s, const std::vector<uint16_t>& st, int a, int b, const Metrics& m) const {
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
    V.resize(lines_.size());
    bool in_code = false;
    float y = 0;
    for (int i = 0; i < (int)lines_.size(); i++) {
        Line& L = V[i];
        const std::string& s = lines_[i];
        uint64_t h = fnv(s);
        if (L.hash != h || L.in_code != in_code || L.wrap != P_.column || L.base != P_.size ||
            L.lh != P_.line_height || L.tab != P_.tab_size || L.blank != P_.blank_line) {
            L.hash = h;
            L.in_code = in_code;
            L.wrap = P_.column;
            L.base = P_.size;
            L.lh = P_.line_height;
            L.tab = P_.tab_size;
            L.blank = P_.blank_line;
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
            const std::vector<uint16_t>& st = mode ? L.rstyle : L.style;

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
        }
        L.pad = 0;
        if (L.info.kind == LineKind::Heading && i > 0)
            L.pad = std::round(P_.size * (L.info.level <= 2 ? 0.5f : 0.25f));
        L.y = y;
        L.h = L.pad + L.rows.size() * L.row_h;
        y += L.h;
        if (L.info.kind == LineKind::Fence) in_code = !in_code;
    }
    total_h_[mode] = y;
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
        if (V[mid].y <= y) lo = mid;
        else hi = mid - 1;
    }
    return std::max(lo, 0);
}

int Editor::col_at_x(int mode, int line, int row, float x) const {
    const Line& L = lay_[mode][line];
    const std::string& s = text_of(mode, line);
    const std::vector<uint16_t>& st = style_of(mode, line);
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
    int r = row_of(L, p.col);
    Metrics m = metrics(L.size);
    float x = row_x(L, r) + measure(lines_[p.line], L.style, L.rows[r], p.col, m);
    return ImVec2(x, L.y + L.pad + r * L.row_h);
}

bool Editor::hit(int mode, ImVec2 d, int& line, int& col) const {
    auto& V = lay_[mode];
    if (V.empty()) return false;
    if (d.y < 0) { line = 0; col = 0; return true; }
    line = line_at_y(mode, d.y);
    const Line& L = V[line];
    if (d.y >= L.y + L.h && line == (int)V.size() - 1) { col = (int)text_of(mode, line).size(); return true; }
    int r = std::clamp((int)std::floor((d.y - L.y - L.pad) / L.row_h), 0, (int)L.rows.size() - 1);
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
    std::vector<uint16_t> st;
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
    if (target.line < 0) target = {line, col_at_x(0, line, r, want_x_)};
    float keep = want_x_;
    move_to(target, select);
    want_x_ = keep;
}

void Editor::select_word_at(TextPos p) {
    const std::string& s = lines_[p.line];
    int n = (int)s.size();
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
    if (has_selection() && cur_.line == anchor_.line) {
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
    replace_selection(s);
    end_edit();
}

void Editor::newline() {
    begin_edit(Edit::Other);
    delete_selection();
    const std::string s = lines_[cur_.line];
    LineInfo info;
    std::vector<uint16_t> st;
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
    end_edit();
}

void Editor::backspace(bool word) {
    if (has_selection()) {
        begin_edit(Edit::Other);
        delete_selection();
        end_edit();
        return;
    }
    if (cur_.line == 0 && cur_.col == 0) return;
    begin_edit(Edit::Delete);
    const std::string& s = lines_[cur_.line];
    TextPos from;
    if (cur_.col == 0) {
        from = {cur_.line - 1, (int)lines_[cur_.line - 1].size()};
    } else {
        LineInfo info;
        std::vector<uint16_t> st;
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
    end_edit();
}

void Editor::del(bool word) {
    if (has_selection()) {
        begin_edit(Edit::Other);
        delete_selection();
        end_edit();
        return;
    }
    TextPos to = right_of(cur_, word);
    if (to == cur_) return;
    begin_edit(Edit::Delete);
    erase(cur_, to);
    anchor_ = cur_;
    end_edit();
}

void Editor::tab() {
    LineInfo info;
    std::vector<uint16_t> st;
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
    auto K = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    if (!alt) {
        if (K(ImGuiKey_LeftArrow)) {
            if (has_selection() && !shift) move_to(std::min(cur_, anchor_), false);
            else move_to(left_of(cur_, ctrl), shift);
        }
        if (K(ImGuiKey_RightArrow)) {
            if (has_selection() && !shift) move_to(std::max(cur_, anchor_), false);
            else move_to(right_of(cur_, ctrl), shift);
        }
        float row = std::round(P_.size / kEm * P_.line_height);
        if (K(ImGuiKey_UpArrow)) {
            if (ctrl) { scroll_target_ -= row; scroll_activity_ = ImGui::GetTime(); }
            else move_vertical(-1, shift);
        }
        if (K(ImGuiKey_DownArrow)) {
            if (ctrl) { scroll_target_ += row; scroll_activity_ = ImGui::GetTime(); }
            else move_vertical(1, shift);
        }
        int page = std::max(1, (int)(view_h_ / row) - 2);
        if (K(ImGuiKey_PageUp)) { scroll_target_ -= page * row; move_vertical(-page, shift); }
        if (K(ImGuiKey_PageDown)) { scroll_target_ += page * row; move_vertical(page, shift); }
        if (K(ImGuiKey_Home)) {
            float keep = -1;
            if (ctrl) move_to({0, 0}, shift);
            else move_to(smart_home(), shift);
            want_x_ = keep;
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
            std::vector<uint16_t> st;
            md_analyze(lines_[line], lay_[0].size() == lines_.size() ? lay_[0][line].in_code : in_code_before(line), info, st, &links);
            for (auto& l : links)
                if (src_col >= l.begin && src_col < l.end) hover_link = &l;
        }
    }
    if (hovered) {
        if ((hover_link && link_mod) || on_box) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        else if (!reader_) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
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

    layout(0);
    if (mode) layout(1);
    if (yoff_.size() != lines_.size()) yoff_.assign(lines_.size(), 0);

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
    if (version_ != before) {
        layout(0);
        if (mode) layout(1);
        if (yoff_.size() != lines_.size()) yoff_.assign(lines_.size(), 0);
    }

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
        if (reveal_mode_ == 1) {
            scroll_target_ = L.y + p.top_pad - view_h_ * 0.22f;
        } else if (reveal_mode_ == 2) {
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
                float ly = origin_y_ + L.y + L.pad + yoff_[l];
                if (ly > p.max.y || ly + L.h < p.min.y) continue;
                Metrics m = metrics(L.size);
                int la = l == a.line ? a.col : 0;
                int lb = l == b.line ? b.col : (int)lines_[l].size();
                for (int r = 0; r < (int)L.rows.size(); r++) {
                    int ra = L.rows[r];
                    int rb = r + 1 < (int)L.rows.size() ? L.rows[r + 1] : (int)lines_[l].size();
                    int sa = std::max(la, ra), sb = std::min(lb, rb);
                    bool eol = l != b.line && r + 1 == (int)L.rows.size();
                    if (sa > sb || (sa == sb && !eol)) continue;
                    float t1 = row_x(L, r) + measure(lines_[l], L.style, ra, sa, m);
                    float t2 = row_x(L, r) + measure(lines_[l], L.style, ra, sb, m);
                    if (eol) t2 += m.sans->GetCharAdvance(' ');
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
                }
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
            float ry = origin_y_ + L.y + L.pad + yoff_[s.line] + s.row * L.row_h;
            if (ry > p.max.y || ry + L.row_h < p.min.y || s.x2 - s.x1 < 0.5f) continue;
            dl->AddRectFilled(ImVec2(std::round(col_x_ + s.x1), ry), ImVec2(std::round(col_x_ + s.x2), ry + L.row_h),
                              fade(col_alpha(T.selection, s.a)));
        }
    }

    int first = V.empty() ? 0 : line_at_y(mode, std::max(0.0f, scroll_ - p.top_pad - view_h_ * 0.5f));
    for (int i = first; i < (int)V.size(); i++) {
        if (origin_y_ + V[i].y - view_h_ * 0.5f > p.max.y) break;
        draw_line(dl, mode, i, col_x_, origin_y_);
    }

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
        bool block = p.caret == EditorParams::Caret::Block;
        float tw = caret_width(p.caret, p.size, p.dpi);
        if (block) {
            if (cur_.col < (int)line.size()) {
                int len;
                unsigned cp = utf8::decode(line.data() + cur_.col, line.data() + line.size(), &len);
                tw = cp == '\t' ? m.tabw : advance(m, cp, L.style[cur_.col]);
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
        bool on = !p.blink || !p.keyboard || std::fmod(now - activity_, 1.1) < 0.6;
        if (on) {
            ImU32 col = p.keyboard ? T.accent : col_alpha(T.accent, 0.4f);
            caret_.draw(dl, ImVec2(col_x_, origin_y_), fade(col), block ? std::round(p.dpi) : 0.0f);

            // Knock the covered character out of the block once it has
            // settled, so it stays readable.
            bool settled = caret_.settled();
            if (block && settled && p.keyboard && cur_.col < (int)line.size() && line[cur_.col] != '\t') {
                uint16_t f = L.style[cur_.col];
                bool mono = is_mono(f), bold = bold_of(f);
                ImFont* bold_font = mono ? p.mono_bold : p.sans_bold;
                ImFont* font = bold && bold_font ? bold_font : (mono ? p.mono : p.sans);
                float size = mono ? m.msize : m.size;
                float row_top = origin_y_ + c.y + yoff_[cur_.line];
                float text_y = row_top + (L.row_h - L.size) * 0.5f + (mono ? (L.size - m.msize) * 0.78f : 0.0f);
                int end = utf8::next(line, cur_.col);
                dl->AddText(font, size, ImVec2(std::round(col_x_ + tx), std::round(text_y)), fade(T.bg),
                            line.data() + cur_.col, line.data() + end);
            }
        }
    }

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

void Editor::draw_line(ImDrawList* dl, int mode, int i, float x0, float y0) {
    const Line& L = lay_[mode][i];
    const std::string& s = text_of(mode, i);
    const std::vector<uint16_t>& st = style_of(mode, i);
    float y = y0 + L.y + L.pad + yoff_[i];
    line_mark_a_ = mode == 0 && syntax_ == Syntax::Markdown && i < (int)mark_a_.size() ? mark_a_[i] : 1.0f;
    Metrics m = metrics(L.size);
    float pad_x = std::round(P_.size * 0.6f);
    float block_h = L.rows.size() * L.row_h;

    if (L.info.kind == LineKind::CodeBlock || L.info.kind == LineKind::Fence)
        dl->AddRectFilled(ImVec2(x0 - pad_x, y), ImVec2(x0 + P_.column + pad_x, y + block_h), fade(T.code_bg));

    if (L.info.kind == LineKind::Rule) {
        float mid = std::round(y + L.row_h * 0.5f);
        float from = mode ? x0 : x0 + measure(s, st, 0, (int)s.size(), m) + L.size * 0.6f;
        dl->AddRectFilled(ImVec2(from, mid), ImVec2(x0 + P_.column, mid + std::max(1.0f, P_.dpi)), fade(T.border));
    }

    // Line number in the left margin, only where there's room.
    if (mode == 0 && P_.line_numbers && x0 - P_.min.x > P_.size * 4) {
        float ns = std::round(P_.size * 0.8f);
        char buf[16];
        snprintf(buf, sizeof buf, "%d", i + 1);
        ImVec2 sz = P_.mono->CalcTextSizeA(ns, FLT_MAX, 0, buf);
        float ty = y + (L.row_h - L.size) * 0.5f + (L.size - ns) * 0.78f;
        ImU32 col = i == cur_.line ? T.dim : T.faint;
        dl->AddText(P_.mono, ns, ImVec2(std::round(x0 - P_.size * 1.3f - sz.x), std::round(ty)), fade(col), buf);
    }

    for (int r = 0; r < (int)L.rows.size(); r++) {
        int a = L.rows[r];
        int b = r + 1 < (int)L.rows.size() ? L.rows[r + 1] : (int)s.size();
        draw_row(dl, mode, L, m, s, st, a, b, x0 + row_x(L, r), y + r * L.row_h);
    }

    // A problem on this line: a mark in the margin and the message after
    // the text, where the eye already is.
    if (mode == 0) {
        auto d = diag_.find(i);
        if (d != diag_.end()) {
            float bw = std::max(2.0f, std::round(2 * P_.dpi));
            float mx = std::round(x0 - P_.size * 0.7f);
            dl->AddRectFilled(ImVec2(mx, y + L.row_h * 0.2f), ImVec2(mx + bw, y + block_h - L.row_h * 0.2f), fade(T.text));
            int last = L.rows.back();
            float ex = x0 + row_x(L, (int)L.rows.size() - 1) + measure(s, st, last, (int)s.size(), m) + P_.size * 1.2f;
            float ms = std::round(P_.size * 0.86f);
            float ry = y + (L.rows.size() - 1) * L.row_h;
            float ty = std::round(ry + (L.row_h - ms) * 0.5f + (m.size - ms) * 0.3f);
            dl->PushClipRect(ImVec2(ex, ry), ImVec2(P_.max.x - P_.size, ry + L.row_h), true);
            dl->AddText(P_.sans, ms, ImVec2(std::round(ex), ty), fade(col_mix(T.dim, T.text, 0.45f)), d->second.c_str());
            dl->PopClipRect();
        }
    }
}

namespace {

struct Look {
    ImU32 col;
    bool bold = false, italic = false, strike = false, underline = false;
};

Look resolve(uint16_t f, const LineInfo& info, bool reader) {
    Look k{T.text};
    if (f & S_Heading) { k.col = T.bright; k.bold = true; }
    if (f & S_Quote) { k.col = col_mix(T.text, T.dim, 0.35f); k.italic = true; }
    if (f & S_Bold) { k.col = T.bright; k.bold = true; }
    if (f & S_Italic) k.italic = true;
    if (f & S_Strike) { k.strike = true; k.col = T.dim; }
    if (f & S_Highlight) k.col = T.bright;
    // Structural markers (bullets, >, [ ]) sit one step above other syntax.
    ImU32 structural = col_mix(T.dim, T.text, 0.45f);
    if (f & (S_Link | S_WikiLink)) { k.col = T.bright; k.underline = true; }
    if (f & (S_Code | S_CodeBlock)) { k.col = T.code; k.bold = k.italic = false; }
    if (f & S_TaskDone) { k.col = T.dim; k.strike = !(f & S_Marker); }
    if (f & S_ListMark) k.col = structural;
    if (f & S_Marker) {
        k.col = T.dim;
        k.bold = k.underline = k.strike = k.italic = false;
        if (f & (S_ListMark | S_QuoteMark | S_TaskBox)) k.col = structural;
        if ((f & S_TaskBox) && info.done) k.col = T.dim;
        if (f & S_Url) k.col = T.faint;
    }
    if (reader && (f & S_ListMark)) k.col = T.dim;
    return k;
}

// delta.ini: sections stand out, values read brighter than their keys,
// syntax and comments step back.
Look resolve_ini(uint16_t f) {
    Look k{T.text};
    if (f & I_Comment) k.col = T.dim;
    else if (f & I_Section) { k.col = T.bright; k.bold = true; }
    else if (f & (I_Bracket | I_Eq)) k.col = T.faint;
    else if (f & I_Value) k.col = T.bright;
    else if (f & I_Key) k.col = col_mix(T.text, T.dim, 0.2f);
    return k;
}

void put_text(ImDrawList* dl, ImFont* font, float size, float x, float y, ImU32 col, const char* a, const char* b,
              bool bold, bool italic) {
    int v0 = dl->VtxBuffer.Size;
    ImVec2 pos(std::round(x), std::round(y));
    dl->AddText(font, size, pos, col, a, b);
    if (bold) dl->AddText(font, size, ImVec2(pos.x + std::max(0.5f, size * 0.035f), pos.y), col, a, b);
    if (italic) {
        float mid = pos.y + size * 0.6f;
        for (int k = v0; k < dl->VtxBuffer.Size; k++) dl->VtxBuffer[k].pos.x += (mid - dl->VtxBuffer[k].pos.y) * 0.19f;
    }
}

}  // namespace

void Editor::draw_row(ImDrawList* dl, int mode, const Line& L, const Metrics& m, const std::string& s,
                      const std::vector<uint16_t>& st, int a, int b, float x0, float ry) {
    const char* p = s.data();
    float ty = ry + (L.row_h - L.size) * 0.5f;
    float mono_ty = ty + (L.size - m.msize) * 0.78f;
    float dpi = P_.dpi;

    // Pass 1: inline code / highlight pills.
    {
        float x = x0;
        int i = a;
        while (i < b) {
            uint16_t f = st[i];
            int key = (f & S_Code) ? 1 : (f & S_Highlight) ? 2 : 0;
            float w = 0;
            int j = i;
            while (j < b) {
                uint16_t g = st[j];
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
        uint16_t f = st[i];
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
        // Structural markers (bullets, >, [ ]) are part of reading a list, so
        // only the other syntax fades away from the caret.
        if ((f & S_Marker) && !(f & (S_ListMark | S_QuoteMark | S_TaskBox)))
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
                     bold && !bold_font, k.italic);
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
