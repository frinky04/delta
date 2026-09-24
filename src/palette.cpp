#include "palette.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "theme.h"
#include "util.h"

bool fuzzy_match(const std::string& pattern, const std::string& text, int& score, std::vector<int>* pos) {
    score = 0;
    if (pattern.empty()) return true;
    std::string p = lower(pattern), t = lower(text);

    // A contiguous hit beats any scattered one.
    size_t sub = t.find(p);
    if (sub != std::string::npos) {
        bool word_start = sub == 0 || !std::isalnum((unsigned char)t[sub - 1]);
        score = 1000 + (word_start ? 300 : 0) + (sub == 0 ? 200 : 0) - (int)t.size();
        if (pos) for (size_t i = 0; i < p.size(); i++) pos->push_back((int)(sub + i));
        return true;
    }

    size_t ti = 0;
    int last = -2;
    for (size_t pi = 0; pi < p.size(); pi++) {
        if (p[pi] == ' ') continue;
        size_t found = t.find(p[pi], ti);
        if (found == std::string::npos) return false;
        // Prefer a later occurrence at a word start if one is close.
        for (size_t k = found; k < t.size() && k < found + 12; k++) {
            if (t[k] == p[pi] && (k == 0 || !std::isalnum((unsigned char)t[k - 1]))) { found = k; break; }
        }
        bool ws = found == 0 || !std::isalnum((unsigned char)t[found - 1]);
        score += 10 + (ws ? 20 : 0) + ((int)found == last + 1 ? 15 : 0) - (int)std::min<size_t>(found - ti, 10);
        if (pos) pos->push_back((int)found);
        last = (int)found;
        ti = found + 1;
    }
    score -= (int)t.size() / 4;
    return true;
}

// ---------------------------------------------------------------------------

void Palette::set_text(const std::string& t) {
    std::snprintf(buf_, sizeof buf_, "%s", t.c_str());
    pending_text_ = t;
    apply_text_ = true;
    built_for_ = "\x01";
}

void Palette::set_wide(bool w) {
    if (w == wide_) return;
    wide_ = w;
    built_for_ = "\x01";
}

void Palette::open(const std::string& text, bool wide) {
    if (!open_) {
        // Reopened while still fading out (a pick that asks something
        // next): the page never came back, so it mustn't flash in now.
        handoff_ = close_t_ < 1.0f;
        open_time_ = anim::clock;
        snap_ = true;
        // Only a real mouse move should pick a row, not wherever it rests.
        last_mouse_ = ImGui::GetIO().MousePos;
        bar_.reset();  // the invert bar and caret start in place, not gliding in
        caret_.reset();
    }
    kind_ = Kind::Query;
    opts_ = {};
    generation_++;
    open_frame_ = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
    keep_sel_.clear();
    open_ = true;
    hold_ = false;
    committed_ = false;
    focus_ = true;
    close_t_ = 1.0f;
    title_.clear();
    // A mode prefix becomes the prompt instead of sitting in the text box.
    mode_ = !text.empty() && std::strchr(kModes, text[0]) ? text[0] : 0;
    wide_ = wide;
    set_text(mode_ ? text.substr(1) : text);
}

void Palette::pick(const std::string& prompt, std::vector<PaletteItem> items, PickOptions opts) {
    open("");
    kind_ = Kind::Pick;
    title_ = prompt;
    fixed_ = std::move(items);
    opts_ = std::move(opts);
}

void Palette::hold(const std::string& prompt, std::vector<PaletteItem> items, int sel) {
    for (int k = 0; k < (int)items.size(); k++) items[k].current = k == sel;
    pick(prompt, std::move(items));
    hold_ = true;
}

void Palette::prompt(const std::string& prompt, const std::string& initial, const std::string& action_label,
                     std::function<void(const std::string&)> submit) {
    open("");
    set_text(initial);
    kind_ = Kind::Prompt;
    title_ = prompt;
    action_label_ = action_label;
    submit_ = std::move(submit);
}

void Palette::update_items(std::vector<PaletteItem> items) {
    if (!open_ || kind_ != Kind::Pick) return;
    if (sel_ >= 0 && sel_ < (int)items_.size()) keep_sel_ = items_[sel_].label;
    fixed_ = std::move(items);
    built_for_ = "\x01";  // filtered again next frame
}

void Palette::close() {
    if (!open_) return;
    open_ = false;
    committed_ = false;
    close_t_ = anim::enabled ? 0.0f : 1.0f;
}

void Palette::commit(int row) {
    close();
    committed_ = true;
    committed_row_ = row;
}

int Palette::input_callback(ImGuiInputTextCallbackData* d) {
    auto* p = static_cast<Palette*>(d->UserData);
    if (p->apply_text_) {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, p->pending_text_.c_str());
        d->SelectionStart = d->SelectionEnd = d->CursorPos = d->BufTextLen;
        p->apply_text_ = false;
    }
    p->cursor_ = d->CursorPos;
    return 0;
}

// Matched characters are drawn in the bold weight and their own colour; the
// first `dim` bytes (a note's space, say) in `dim_col` where they aren't.
static void draw_label(ImDrawList* dl, ImFont* font, ImFont* bold, float size, ImVec2 pos, const std::string& s,
                       const std::vector<int>& hl, ImU32 col, ImU32 hl_col, int dim = 0, ImU32 dim_col = 0) {
    std::vector<char> mask(s.size(), 0);  // 0 plain, 1 matched, 2 dim
    for (int k = 0; k < std::min(dim, (int)s.size()); k++) mask[k] = 2;
    for (int h : hl) {
        if (h < 0 || h >= (int)s.size()) continue;
        int e = utf8::next(s, h);
        for (int k = h; k < e; k++) mask[k] = 1;
    }
    float x = pos.x;
    for (size_t i = 0; i < s.size();) {
        size_t j = i;
        while (j < s.size() && mask[j] == mask[i]) j++;
        ImFont* fnt = mask[i] == 1 && bold ? bold : font;
        ImU32 c = mask[i] == 1 ? hl_col : mask[i] == 2 ? dim_col : col;
        dl->AddText(fnt, size, ImVec2(std::round(x), pos.y), c, s.data() + i, s.data() + j);
        x += fnt->CalcTextSizeA(size, FLT_MAX, 0, s.data() + i, s.data() + j).x;
        i = j;
    }
}

// Width of a label as draw_label() sets it (matched characters are bold).
static float label_width(ImFont* font, ImFont* bold, float size, const std::string& s, const std::vector<int>& hl) {
    std::vector<char> mask(s.size(), 0);
    for (int h : hl) {
        if (h < 0 || h >= (int)s.size()) continue;
        int e = utf8::next(s, h);
        for (int k = h; k < e; k++) mask[k] = 1;
    }
    float w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = i;
        while (j < s.size() && mask[j] == mask[i]) j++;
        ImFont* fnt = mask[i] && bold ? bold : font;
        w += fnt->CalcTextSizeA(size, FLT_MAX, 0, s.data() + i, s.data() + j).x;
        i = j;
    }
    return w;
}

std::function<void()> Palette::draw(const Frame& f, const Builder& build, const PromptFor& prompt_for) {
    std::function<void()> chosen;
    if (!open_) {
        if (close_t_ >= 1.0f) { page_a_ = 1.0f; return {}; }
        // A pick takes a little longer: it has a beat where the choice lingers.
        close_t_ = std::min(1.0f, close_t_ + anim::step(f.dt) / ((committed_ ? 0.22f : 0.11f) / anim::speed));
        anim::active = true;
    }

    // The page and the palette take turns, never overlap. Opening: the page
    // fades out, then the palette in. Closing: the palette fades out, then
    // the page in; after a pick, the other rows go first and the chosen row
    // lingers before it goes too.
    auto phase = [](float x, float a, float b) { return anim::ease_out(std::clamp((x - a) / (b - a), 0.0f, 1.0f)); };
    float vis, vis_pick;
    if (open_) {
        float t = anim::ease_out(anim::progress(open_time_, anim::clock, 0.13f));
        if (handoff_) {
            // From one list straight to the next: no fade, like a mode switch.
            page_a_ = 0.0f;
            vis = vis_pick = 1.0f;
        } else {
            page_a_ = t >= 0.5f ? 0.0f : 1.0f - t * 2.0f;
            vis = vis_pick = std::clamp((t - 0.4f) / 0.6f, 0.0f, 1.0f);
        }
    } else if (committed_) {
        vis = 1.0f - phase(close_t_, 0.0f, 0.3f);
        vis_pick = 1.0f - phase(close_t_, 0.3f, 0.55f);
        page_a_ = phase(close_t_, 0.55f, 1.0f);
    } else {
        vis = vis_pick = 1.0f - phase(close_t_, 0.0f, 0.45f);
        page_a_ = phase(close_t_, 0.45f, 1.0f);
    }

    // Backspace on an empty query leaves the mode, like a minibuffer.
    if (open_ && kind_ == Kind::Query && mode_ && buf_[0] == 0 && ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        mode_ = 0;
        built_for_ = "\x01";
    }
    // Tab widens a scoped mode to every space, and narrows it back.
    if (open_ && !hold_ && scoped() && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) set_wide(!wide_);

    // Rebuild the list whenever the text changes.
    if (open_ && built_for_ != text()) {
        built_for_ = text();
        items_.clear();
        total_ = -1;
        empty_.clear();
        if (kind_ == Kind::Query) {
            tag_.clear();
            build(text(), items_, total_);
        } else if (kind_ == Kind::Pick) {
            std::string q = trim(buf_);
            for (auto& it : fixed_) {
                PaletteItem c = it;
                c.hl.clear();
                if (fuzzy_match(q, c.label, c.score, &c.hl)) items_.push_back(c);
            }
            if (!q.empty())
                std::stable_sort(items_.begin(), items_.end(), [](auto& a, auto& b) { return a.score > b.score; });
            total_ = (int)fixed_.size();
            PaletteItem typed;
            if (opts_.typed && opts_.typed(q, typed)) {
                typed.emphasis = true;
                items_.insert(items_.begin(), std::move(typed));
            }
        } else {
            std::string v = trim(buf_);
            PaletteItem it;
            it.label = v.empty() ? action_label_ : action_label_ + " \"" + v + "\"";
            it.emphasis = !v.empty();
            if (!v.empty()) {
                auto submit = submit_;
                it.action = [submit, v] { submit(v); };
            }
            items_.push_back(it);
        }
        sel_ = 0;
        // The row you were on, if the list changed under you; else where you are.
        int keep = -1;
        for (int k = 0; k < (int)items_.size() && !keep_sel_.empty(); k++)
            if (items_[k].label == keep_sel_) { keep = k; break; }
        if (keep >= 0) {
            sel_ = keep;
            reveal_sel_ = true;
        } else {
            scroll_target_ = 0;
            for (int k = 0; k < (int)items_.size(); k++)
                if (items_[k].current) { sel_ = k; reveal_sel_ = true; break; }
        }
        keep_sel_.clear();
    }

    // Set in the page's column like a terminal picker: all mono, a prompt
    // that names the mode, a match count, tabular details on the right.
    const float s = f.size, em = std::max(s, f.text) / kEm;  // grows with text larger than the body
    const float ms = f.text;
    ImFont* mono = f.mono;
    ImFont* mono_b = f.mono_bold ? f.mono_bold : f.mono;
    auto W = [&](ImFont* fnt, const std::string& t) { return fnt->CalcTextSizeA(ms, FLT_MAX, 0, t.c_str()).x; };
    const float x0 = f.col_x, cw = f.col_w;
    const float in_h = std::round(em * 1.6f);
    const float row_h = std::round(em * 1.65f);
    const float list_top = f.top + in_h + std::round(em * 0.9f);
    // The preview line, under a gap as wide as the one above the list.
    const float preview_gap = opts_.preview ? std::round(em * 0.9f) : 0;
    const float preview_h = opts_.preview ? std::round(em * 2.4f) : 0;
    const int fit = std::max(3, (int)((f.bottom - list_top - preview_gap - preview_h) / row_h));
    const int n = (int)items_.size();
    // More than fits: a row fewer, and the top half of the next one shows
    // below, fading out, so the list never looks complete when it isn't.
    const bool more = n > fit;
    const int max_rows = more ? fit - 1 : fit;
    const float peek = more ? std::round(row_h * 0.5f) : 0;
    const int shown = std::max(1, std::min(n, max_rows));
    const float y0 = f.top;
    const float list_y = list_top;
    const float margin = std::round(em * 1.6f);           // room for the > marker
    const bool invert = f.selection == Selection::Invert;

    ImGui::SetNextWindowPos(ImVec2(x0 - margin, y0));
    ImGui::SetNextWindowSize(ImVec2(cw + margin * 2, list_y + shown * row_h + peek + preview_gap + preview_h - y0));
    if (open_ && focus_) ImGui::SetNextWindowFocus();
    ImGui::Begin("##palette", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();

    // Prompt line: "projects/" or "commands: ", the query, then "3/12".
    const float text_y = std::round(y0 + (in_h - ms) * 0.5f);
    std::string prompt = kind_ == Kind::Query ? prompt_for(mode_) : title_;
    float prompt_w = W(mono, prompt);
    dl->AddText(mono, ms, ImVec2(x0, text_y), col_alpha(T.dim, vis), prompt.c_str());
    // Matches over candidates. "+ new ..." rows aren't matches; line and
    // search modes (total -2) don't get a count.
    std::string count;
    int matches = (int)std::count_if(items_.begin(), items_.end(), [](const PaletteItem& it) { return !it.emphasis && it.action; });
    if (kind_ != Kind::Prompt && total_ != -2)
        count = total_ >= 0 ? std::to_string(matches) + "/" + std::to_string(total_) : std::to_string(matches);
    if (kind_ == Kind::Query && !tag_.empty()) count = tag_ + (count.empty() ? "" : "  " + count);
    float count_w = count.empty() ? 0 : W(mono, count);
    if (!count.empty())
        dl->AddText(mono, ms, ImVec2(std::round(x0 + cw - count_w), text_y), col_alpha(T.dim, vis), count.c_str());

    char before[sizeof buf_];
    std::memcpy(before, buf_, sizeof buf_);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, vis);
    ImGui::PushFont(mono, ms);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(T.bright));
    ImGui::PushStyleColor(ImGuiCol_InputTextCursor, ImVec4(0, 0, 0, 0));  // we draw our own below
    ImGui::SetCursorScreenPos(ImVec2(x0 + prompt_w, text_y));
    ImGui::SetNextItemWidth(std::max(em * 4, cw - prompt_w - count_w - em));
    if (open_ && (focus_ || !ImGui::IsAnyItemActive())) ImGui::SetKeyboardFocusHere();
    focus_ = false;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackAlways;
    if (!open_) flags |= ImGuiInputTextFlags_ReadOnly;
    ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, true);  // tab is ours (see hold)
    ImGui::InputText("##q", buf_, sizeof buf_, flags, &Palette::input_callback, this);
    ImGui::PopItemFlag();
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
    ImGui::PopStyleVar();

    // Our caret, the same shape and motion as the editor's: a smear rect
    // at the input's cursor, blinking from the last change.
    {
        int cur = std::clamp(cursor_, 0, (int)std::strlen(buf_));
        if (cur != last_cursor_ || last_buf_ != buf_) {
            caret_activity_ = f.now;
            last_cursor_ = cur;
            last_buf_ = buf_;
        }
        float cx = x0 + prompt_w + mono->CalcTextSizeA(ms, FLT_MAX, 0, buf_, buf_ + cur).x;
        float ch = ms, w = std::round(f.caret_w);
        float cy = std::round(y0 + (in_h - ch) * 0.5f);
        ImVec2 cmin(std::round(cx - std::round(w * 0.4f)), cy), cmax(cmin.x + w, cy + ch);
        if (!caret_.valid()) caret_.snap(cmin, cmax);
        caret_.set(cmin, cmax, false, 50.0f);  // no smear here: it only suits the page
        caret_.update(f.dt);
        bool on = !f.blink || !open_ || std::fmod(f.now - caret_activity_, 1.1) < 0.6;
        if (on) caret_.draw(dl, ImVec2(0, 0), col_alpha(T.accent, vis), 0.0f);
    }

    // Typing a mode character first switches mode; it becomes the prompt.
    if (open_ && kind_ == Kind::Query && mode_ == 0 && before[0] == 0 && buf_[0] && std::strchr(kModes, buf_[0])) {
        mode_ = buf_[0];
        set_text(buf_ + 1);
    }

    // Taking a row: its action, or (for a row that fills the query) its text.
    auto take = [&](int k) {
        if (!items_[k].fill.empty()) {
            const std::string& t = items_[k].fill;
            mode_ = !t.empty() && std::strchr(kModes, t[0]) ? t[0] : 0;
            set_text(mode_ ? t.substr(1) : t);
            return;
        }
        chosen = items_[k].action;
        commit(k);
    };

    // Whatever opened it this frame (an enter in a status-line tool, a click
    // on a #tag) isn't also an answer or a click away.
    const bool fresh = ImGui::GetFrameCount() == open_frame_;

    // Keyboard.
    bool keyed = false;  // the selection moved by key this frame
    int sel_before = sel_;
    if (open_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) close();
        if (hold_ && n > 0 && open_) {
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) sel_ = ImGui::GetIO().KeyShift ? (sel_ - 1 + n) % n : (sel_ + 1) % n;
            if (!ImGui::GetIO().KeyCtrl && items_[sel_].action) {
                chosen = items_[sel_].action;
                commit(sel_);
            }
        }
        if (n > 0 && open_) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % n;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) sel_ = (sel_ - 1 + n) % n;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) sel_ = std::min(n - 1, sel_ + max_rows);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) sel_ = std::max(0, sel_ - max_rows);
            bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
            if (enter && !fresh && items_[sel_].action) take(sel_);
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !fresh) close();
        keyed = sel_ != sel_before;
    }

    // List. Keyboard moves keep the selection a couple of rows from the
    // edge (vim's scrolloff), so the eased scroll always has slack: holding a
    // key never pushes the selection past the visible rows, and scrolling
    // stays smooth. Hover and the mouse wheel don't drag the list around.
    const int off = std::min(2, (max_rows - 1) / 2);
    const float max_scroll = (float)std::max(0, n - max_rows);
    if (keyed || reveal_sel_) {
        if (sel_ < scroll_target_ + off) scroll_target_ = (float)(sel_ - off);
        if (sel_ > scroll_target_ + max_rows - 1 - off) scroll_target_ = (float)(sel_ - max_rows + 1 + off);
        reveal_sel_ = false;
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0) scroll_target_ -= ImGui::GetIO().MouseWheel * 3;
    scroll_target_ = std::clamp(scroll_target_, 0.0f, max_scroll);
    if (snap_) { sel_y_ = (float)sel_; scroll_ = scroll_target_; snap_ = false; }
    scroll_ = anim::approach(scroll_, scroll_target_, 38.0f, f.dt, 0.01f);
    sel_y_ = anim::approach(sel_y_, (float)sel_, 45.0f, f.dt, 0.01f);

    const float row_text_y = std::round((row_h - ms) * 0.5f);
    const float pad = std::round(em * 0.3f);   // inverted bar overhang around the label
    const float list_bottom = list_y + shown * row_h + peek;
    dl->PushClipRect(ImVec2(x0 - margin, list_y), ImVec2(x0 + cw + margin, list_bottom), true);
    if (n == 0) {
        std::string msg = !empty_.empty() ? empty_ : trim(buf_).size() > 1 ? "no matches" : "nothing here yet";
        dl->AddText(mono, ms, ImVec2(x0, list_y + row_text_y), col_alpha(T.dim, vis), msg.c_str());
    } else {
        float pick_a = vis_pick;
        if (!invert) {
            float hy = list_y + (sel_y_ - scroll_) * row_h;
            dl->AddText(mono_b, ms, ImVec2(std::round(x0 - margin * 0.75f), std::round(hy + row_text_y)), col_alpha(T.accent, pick_a), ">");
        }

        ImVec2 mouse = ImGui::GetIO().MousePos;
        bool mouse_moved = mouse.x != last_mouse_.x || mouse.y != last_mouse_.y;
        last_mouse_ = mouse;
        int first = std::max(0, (int)std::floor(scroll_));
        int last = std::min(n - 1, first + max_rows);
        auto row_alpha = [&](int k) { return committed_ && k == committed_row_ ? vis_pick : vis; };
        auto label_x = [&](const PaletteItem& it) { return x0 + it.indent * W(mono, "  "); };
        auto label_end = [&](const PaletteItem& it) { return x0 + cw - (it.meta.empty() ? 0.0f : W(mono, it.meta)) - em; };

        // Rows, all in their normal colours.
        for (int k = first; k <= last; k++) {
            const PaletteItem& it = items_[k];
            float ry = list_y + (k - scroll_) * row_h;
            bool row_hover = hovered && mouse.y >= ry && mouse.y < ry + row_h && mouse.y >= list_y;
            if (row_hover && mouse_moved && open_) sel_ = k;
            if (row_hover && open_ && !fresh && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && it.action) take(k);
            bool on = k == sel_;
            float a = row_alpha(k);
            ImU32 col = on ? T.text : col_mix(T.dim, T.text, 0.55f);
            if (it.emphasis && on) col = T.bright;
            if (!it.action) col = T.dim;
            ImU32 hl = on ? T.bright : T.text;
            ImU32 meta = on ? col_mix(T.dim, T.text, 0.45f) : T.dim;
            if (!it.meta.empty()) {
                float meta_w = W(mono, it.meta);
                dl->AddText(mono, ms, ImVec2(std::round(x0 + cw - meta_w), std::round(ry + row_text_y)), col_alpha(meta, a), it.meta.c_str());
            }
            float lx = label_x(it);
            dl->PushClipRect(ImVec2(lx, ry), ImVec2(label_end(it), ry + row_h), true);
            draw_label(dl, mono, mono_b, ms, ImVec2(lx, ry + row_text_y), it.label, it.hl, col_alpha(col, a), col_alpha(hl, a),
                       it.dim, col_alpha(meta, a));
            if (!it.aside.empty()) {
                float ax = lx + label_width(mono, mono_b, ms, it.label, it.hl) + W(mono, "  ");
                dl->AddText(mono, ms, ImVec2(std::round(ax), std::round(ry + row_text_y)), col_alpha(T.dim, a), it.aside.c_str());
            }
            dl->PopClipRect();
        }

        if (invert) {
            // A bright block sized to the label, like the block caret: same
            // height, same 1px rounding, gliding between rows. It lives in
            // list space (row * row_h) so scrolling doesn't drag it.
            const PaletteItem& it = items_[std::clamp(sel_, 0, n - 1)];
            float tx = label_x(it) - pad;
            float tw = std::min(label_width(mono, mono_b, ms, it.label, it.hl) + pad * 2, label_end(it) + em - tx);
            float bh = ms;
            float by = std::round(sel_ * row_h + (row_h - bh) * 0.5f);
            ImVec2 boff(0, list_y - scroll_ * row_h);
            bar_.set(ImVec2(tx, by), ImVec2(tx + tw, by + bh), false, 45.0f);
            bar_.update(f.dt);
            bar_.draw(dl, boff, col_alpha(T.accent, pick_a), std::round(f.dpi));

            // Then the text under the bar, knocked out in the background
            // colour and clipped to wherever the bar actually is, so mid-glide
            // the inversion follows the bar instead of jumping ahead of it.
            ImVec2 bmin, bmax;
            bar_.bounds(boff, bmin, bmax);
            dl->PushClipRect(bmin, bmax, true);
            for (int k = first; k <= last; k++) {
                const PaletteItem& row = items_[k];
                float ry = list_y + (k - scroll_) * row_h;
                if (ry > bmax.y || ry + row_h < bmin.y) continue;
                float lx = label_x(row);
                dl->PushClipRect(ImVec2(lx, ry), ImVec2(label_end(row), ry + row_h), true);
                ImU32 ink = col_alpha(T.bg, row_alpha(k));
                draw_label(dl, mono, mono_b, ms, ImVec2(lx, ry + row_text_y), row.label, row.hl, ink, ink);
                dl->PopClipRect();
            }
            dl->PopClipRect();
        }
    }
    if (peek > 0) {
        ImU32 clear = col_alpha(T.bg, 0.0f), solid = col_alpha(T.bg, vis);
        dl->AddRectFilledMultiColor(ImVec2(x0 - margin, list_bottom - peek), ImVec2(x0 + cw + margin, list_bottom), clear, clear, solid, solid);
    }
    dl->PopClipRect();

    if (opts_.preview) {
        // A hairline between the list and the preview, like a terminal
        // picker's rule above its preview pane.
        float py = list_bottom + preview_gap;
        float rule = std::round(list_bottom + preview_gap * 0.5f);
        dl->AddLine(ImVec2(x0, rule), ImVec2(x0 + cw, rule), col_alpha(T.border, vis), std::max(1.0f, std::round(f.dpi)));
        dl->PushClipRect(ImVec2(x0, py), ImVec2(x0 + cw, py + preview_h), true);
        opts_.preview(n > 0 ? &items_[std::clamp(sel_, 0, n - 1)] : nullptr, dl, ImVec2(x0, py), cw, preview_h, vis);
        dl->PopClipRect();
    }
    dl->PopClipRect();
    ImGui::End();
    if (open_ && opts_.select) opts_.select(n > 0 ? &items_[std::clamp(sel_, 0, n - 1)] : nullptr);
    return chosen;
}

