// The status line as a minibuffer (like vim's / or Emacs's C-s): tools that
// act on the note in view happen down there, so the note stays in full
// view. The bar and its fields, and its tools: find (and replace), go to
// line, and the prompt that renames a note or a tag. Fixing spelling is a
// bar too, with the rest of spelling (app_spell.cpp).
#include <algorithm>
#include <cmath>

#include "app.h"
#include "theme.h"
#include "util.h"

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
    // Where find or go to line took you is a jump (escape from go to line
    // has already gone back, so it isn't).
    if (bar_ == Bar::Find || bar_ == Bar::Line) history_.jumped(bar_from_, here());
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
    // Text set while the field is being edited (ImGui keeps its own copy).
    if (app->bar_set_pending_) {
        d->DeleteChars(0, d->BufTextLen);
        d->InsertChars(0, app->bar_set_.c_str());
        app->bar_set_pending_ = false;
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

// Enter on the bar, either key; `repeat` false ignores it held down.
bool App::enter_pressed(bool repeat) {
    return ImGui::IsKeyPressed(ImGuiKey_Enter, repeat) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, repeat);
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
        case Bar::Prompt: draw_prompt_bar(min, max, s, pad, ty); break;
        case Bar::Fix: draw_fix_bar(min, max, s, pad, ty); break;
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
        bar_from_ = here();
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
    bool enter = enter_pressed();
    if (enter && io.KeyCtrl && io.KeyAlt && find_replace_) {
        int n = editor_.replace_all(repl_buf_);
        status(n ? "replaced " + std::to_string(n) : "nothing to replace");
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

void App::open_line(const std::string& text) {
    if (!note_) return;
    bool fresh = bar_ != Bar::Line;
    if (fresh) {
        bar_cur_ = editor_.cursor();
        bar_anchor_ = editor_.anchor();
        bar_scroll_ = editor_.scroll();
    }
    open_bar(Bar::Line);
    if (fresh) bar_from_ = here();
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
    bool enter = enter_pressed();
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

// A prompt (see BarPrompt): enter checks the text, then acts on it; a reason
// it can't shows at the right until the text changes.

void App::open_prompt(BarPrompt p, const std::string& initial) {
    open_bar(Bar::Prompt);
    prompt_ = std::move(p);
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", initial.c_str());
    bar_last_ = bar_buf_;
}

void App::draw_prompt_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };
    if (prompt_.note && !note_) { close_bar(); return; }

    if (enter_pressed()) {
        std::string v = trim(bar_buf_);
        std::string e = prompt_.problem(v);
        if (e.empty()) e = prompt_.submit(v);
        if (e.empty()) { close_bar(); return; }
        bar_error_ = e;
        bar_focus_ = true;
        bar_select_pending_ = 2;
    }

    float x = min.x + pad;
    for (auto& [text, colour] : prompt_.lead) {
        dl->AddText(mono_, s, ImVec2(x, ty), T.*colour, text.c_str());
        x += W(text);
    }
    float w = std::clamp(W(bar_buf_) + s * 2, s * 12, (max.x - min.x) * 0.45f);
    bar_input("##prompt", bar_buf_, sizeof bar_buf_, 1, x, ty, w);

    if (bar_last_ != bar_buf_) { bar_last_ = bar_buf_; bar_error_.clear(); }
    std::string v = trim(bar_buf_);
    std::string msg = bar_error_.empty() ? prompt_.problem(v) : bar_error_;
    std::string right = msg.empty() ? prompt_.hint(v) : msg;
    float rx = std::max(x + w + s * 1.6f, max.x - pad - W(right));
    dl->PushClipRect(ImVec2(x + w, min.y), ImVec2(max.x - pad, max.y), true);
    dl->AddText(mono_, s, ImVec2(rx, ty), msg.empty() ? T.dim : T.text, right.c_str());
    dl->PopClipRect();
}
