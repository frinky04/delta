// Marks in the note: misspelt words, checked as you type, and the other
// problems found in a document (a [[link]] to nowhere, a config line delta
// can't read). Stepping through them, the status line's echo of the one at
// the caret, and fixing a word: in place (the quick fix) or a word at a time
// in the status line's fix bar. The speller is speller.h's; the fixes' text
// work is spell_fix.h's.
#include <algorithm>
#include <sstream>

#include "app.h"
#include "problems.h"
#include "spell_fix.h"
#include "theme.h"
#include "util.h"

// Stepping through marks in text order from the selection (`a` to `b`):
// the first after it, or the last before it, wrapping round.
static const Diagnostic& step_from(const std::vector<Diagnostic>& marks, TextPos a, TextPos b, int dir) {
    auto pos = [](const Diagnostic& d) { return TextPos{d.line, std::max(0, d.col0)}; };
    if (dir > 0) {
        TextPos from = std::max(a, b);
        for (auto& d : marks)
            if (from < pos(d)) return d;
        return marks.front();
    }
    TextPos from = std::min(a, b);
    for (auto it = marks.rbegin(); it != marks.rend(); ++it)
        if (pos(*it) < from) return *it;
    return marks.back();
}

// Gathers the open document's problems from the checks that apply to it:
// the config's own, or those found in a note. Kept in line order, for the
// count and F8.
void App::refresh_problems() {
    problems_.clear();
    if (is_config()) {
        for (auto& is : config_problems_)
            if (is.line >= 0) problems_.push_back({is.line, is.col0, is.col1, is.msg});
    } else if (note_) {
        problems_ = link_problems(editor_.lines(), note_index_);
    }
    std::stable_sort(problems_.begin(), problems_.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col0 < b.col0;
    });
    diag_gen_++;
    lint_version_ = UINT64_MAX;  // another note may have this version too
    check_lint();
}

// The words in words.txt, one a line ("#" starts a comment line).
static std::vector<std::string> read_words(const fs::path& path) {
    std::vector<std::string> out;
    std::string text;
    if (!read_file(path, text)) return out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

// Loads the dictionaries the open space asks for, if they aren't what's
// loaded already (or `force`: words.txt changed).
void App::apply_spelling(bool force) {
    std::string langs = cfg_.spell_languages;
    if (auto it = cfg_.spell_spaces.find(lower(space_)); it != cfg_.spell_spaces.end()) langs = it->second;
    std::string key = cfg_.spell ? langs : "";
    if (!force && key == spell_key_) return;
    // The same dictionaries again (words.txt changed): the marks stay up
    // until the reload is in, rather than blinking out.
    if (key != spell_key_) {
        spelling_.clear();
        diag_gen_++;
    }
    spell_key_ = key;
    spell_version_ = UINT64_MAX;
    spell_pending_ = false;
    words_mtime_ = mtime_of(words_path_);
    if (!cfg_.spell) { speller_.unload(); return; }
    std::vector<std::string> words = read_words(words_path_);
    speller_.load(split_list(langs), dict_dir_, words);
    spell_report_ = true;
}

// Spelling marks for the open note. New lines are checked a few
// milliseconds' worth a frame, so a long note never stalls one.
void App::check_spelling() {
    if (!is_note() || !cfg_.spell || !speller_.ready()) return;
    if (spell_report_) {
        spell_report_ = false;
        if (!speller_.errors().empty()) status("dictionary " + speller_.errors().front(), true);
    }
    if (editor_.version() == spell_version_ && speller_.generation() == spell_gen_ && !spell_pending_) return;
    spell_version_ = editor_.version();
    spell_gen_ = speller_.generation();
    spell_pending_ = !speller_.check(editor_.lines(), spelling_, 0.004);
    diag_gen_++;
}

// The misspelt word being typed isn't marked until the caret leaves it.
// Returns its start (line -1: none).
TextPos App::spell_held() {
    if (editor_.version() != edit_version_) {
        edit_version_ = editor_.version();
        spell_hold_ = editor_.cursor();
    }
    if (spell_hold_.line < 0) return {-1, 0};
    TextPos c = editor_.cursor();
    if (int i = misspelling_at(spelling_, spell_hold_); i >= 0) {
        const Diagnostic& d = spelling_[i];
        if (c.line == d.line && d.col0 <= c.col && c.col <= d.col1) return {d.line, d.col0};
    }
    if (c != spell_hold_) spell_hold_.line = -1;  // moved on
    return {-1, 0};
}

// Hands the problems and spelling marks to the editor, when they've changed.
// Problems on the line being typed on wait for a pause, so a half-typed
// value or [[link]] doesn't flash at you; a word waits for you to finish it.
void App::show_problems(double now) {
    bool typing = now - last_change_ < 0.9;
    if (typing) next_problem_check_ = last_change_ + 0.9;
    int hold = typing ? editor_.cursor().line : -1;
    TextPos held = spell_held();
    TextPos lint_held = this->lint_held();
    if (diag_gen_ == shown_gen_ && hold == shown_hold_ && held == shown_held_ && lint_held == shown_lint_held_) return;
    shown_gen_ = diag_gen_;
    shown_hold_ = hold;
    shown_held_ = held;
    shown_lint_held_ = lint_held;
    std::vector<Diagnostic> d;
    for (auto& p : problems_)
        if (p.line != hold) d.push_back(p);
    for (auto& p : spelling_)
        if (p.line != held.line || p.col0 != held.col) d.push_back(p);
    add_lint_marks(d, hold, lint_held);
    editor_.set_diagnostics(d);
}

const Diagnostic* App::misspelt_at_caret() const {
    int i = misspelling_at(spelling_, editor_.cursor());
    return i >= 0 ? &spelling_[i] : nullptr;
}

// What the status line echoes about the caret's spot: a misspelt word's
// likeliest fixes, or the message of a problem in a note. Nothing pops up
// over the text; it's there if you look down.
void App::update_echo() {
    echo_.clear();
    if (!note_ || editor_.reader()) return;
    TextPos c = editor_.cursor();
    if (const Diagnostic* d = misspelt_at_caret(); d && !(shown_held_.line == d->line && shown_held_.col == d->col0)) {
        // Only the best guess ("Disempower?"); the key for the rest (and for
        // keeping the word) is an aside.
        // Found in the background (they can take tens of milliseconds): the
        // echo shows once they're in.
        const std::string& s = editor_.lines()[d->line];
        auto found = speller_.suggest_later(spell_form(s, {d->col0, d->col1}));
        if (!found) return;
        const auto& sug = *found;
        using Act = Echo::Act;
        if (sug.empty()) {
            echo_.push_back({"no suggestions", Tone::Plain});
        } else {
            echo_.push_back({sug[0], Tone::Strong, Act::QuickFix});
            echo_.push_back({"?", Tone::Plain, Act::QuickFix});
        }
        // The keys: "alt+. fix · ctrl+. more" (either may be unbound).
        std::string quick = sug.empty() ? "" : chord_for("edit.quick_fix");
        std::string more = chord_for("edit.fix_spelling");
        if (!quick.empty() || !more.empty()) echo_.push_back({"   ", Tone::Quiet});
        if (!quick.empty()) echo_.push_back({quick + " fix", Tone::Quiet, Act::QuickFix});
        if (!quick.empty() && !more.empty()) echo_.push_back({" \xC2\xB7 ", Tone::Quiet});
        if (!more.empty()) echo_.push_back({quick.empty() ? more : more + " more", Tone::Quiet, Act::FixSpelling});
    } else if (!is_config() && c.line != shown_hold_) {
        // The config's problems say it at the ends of their lines, which are
        // short and right beside them.
        for (auto& p : problems_)
            if (p.line == c.line && p.col0 >= 0 && p.col0 <= c.col && c.col <= p.col1) {
                echo_.push_back({p.msg, Tone::Strong});
                break;
            }
        // A style slip says what it is, and the key that fixes it.
        const LintProblem* l = echo_.empty() ? lint_at_caret() : nullptr;
        if (l) {
            echo_.push_back({l->msg, Tone::Strong});
            std::string key = l->fixable ? chord_for("edit.fix_slip") : "";
            if (!key.empty()) {
                echo_.push_back({"   ", Tone::Quiet});
                echo_.push_back({key + " fix", Tone::Quiet, Echo::Act::FixSlip});
            }
        }
    }
}

void App::goto_misspelling(int dir) {
    Jump j(*this, History::Misspelling);
    if (spelling_.empty()) {
        status(!cfg_.spell ? "spellcheck is off" : speller_.loading() || spell_pending_ ? "still checking the spelling" : "no misspelt words in this note");
        return;
    }
    const Diagnostic& to = step_from(spelling_, editor_.cursor(), editor_.anchor(), dir);
    editor_.select_range({to.line, to.col0}, {to.line, to.col1});
}

// The quick fix: the misspelt word at the caret, or the nearest one before
// it in the same paragraph, takes its best guess, and the caret stays where
// it was. Pressed again straight away, the next guess goes in instead (and
// at the end of them, the word as it was); one undo puts the word back,
// whichever it's on.
void App::quick_fix() {
    if (!is_note() || editor_.reader()) return;
    check_spelling();  // marks for the text as it is now, not last frame's
    if (quick_.line >= 0 && editor_.version() == quick_.version && editor_.cursor() == quick_.caret) {
        quick_.pick = (quick_.pick + 1) % (int)quick_.options.size();
        editor_.undo();  // back to the word as written, then the next guess
        quick_apply();
        return;
    }
    quick_.line = -1;
    TextPos c = editor_.cursor();
    const auto& lines = editor_.lines();
    int near = misspelling_near(spelling_, lines, c);
    const Diagnostic* d = near >= 0 ? &spelling_[near] : nullptr;
    if (!d || d->col1 > (int)lines[d->line].size()) { status(cfg_.spell ? "no misspelt word here" : "spellcheck is off"); return; }
    std::string word = lines[d->line].substr(d->col0, d->col1 - d->col0);
    const auto& sugg = speller_.suggest(spell_form(lines[d->line], {d->col0, d->col1}));
    if (sugg.empty()) { status("no suggestions for " + word); return; }
    quick_.line = d->line;
    quick_.col0 = d->col0;
    quick_.word = word;
    quick_.options = sugg;
    quick_.options.push_back(word);  // the cycle ends on the word as it was
    quick_.pick = 0;
    quick_.from = c;
    quick_apply();
}

// Puts the picked guess in place of the word, the caret back where it was
// (moved along if the word before it changed length).
void App::quick_apply() {
    const std::string& to = quick_.options[quick_.pick];
    int end = quick_.col0 + (int)quick_.word.size();
    editor_.replace_range({quick_.line, quick_.col0}, {quick_.line, end}, to);
    TextPos back = after_replace(quick_.from, quick_.line, quick_.col0, (int)quick_.word.size(), (int)to.size());
    editor_.jump_to(back);
    quick_.caret = back;
    quick_.version = editor_.version();
    // No message: the change pulses where it happened, and a message would
    // cover the echo for the next word.
}

// Fixing spelling happens in the status line, a word at a time. The field
// holds the fix: the best guess to start with, tab for the others, or type
// your own. Enter fixes the word and goes on to the next, shift+enter skips
// it, ctrl+enter adds it to your dictionary; the pass ends where it began,
// or at escape. From a word that isn't misspelt, it starts at the next one.
void App::fix_spelling() {
    if (!is_note()) return;
    if (editor_.reader()) editor_.set_reader(false);
    check_spelling();  // marks for the text as it is now, not last frame's
    const Diagnostic* d = misspelt_at_caret();
    if (!d) {
        goto_misspelling(1);  // says why not, if there's nowhere to go
        if (!(d = misspelt_at_caret())) return;
    }
    fix_origin_ = {d->line, d->col0};
    fix_wrapped_ = fix_advance_ = false;
    open_bar(Bar::Fix);
    fix_load(*d);
}

// Takes up a misspelt word: selected in the note, its best guess in the field.
void App::fix_load(const Diagnostic& d) {
    const std::string& s = editor_.lines()[d.line];
    fix_.line = d.line;
    fix_.col0 = d.col0;
    fix_.col1 = d.col1;
    fix_.word = s.substr(d.col0, d.col1 - d.col0);
    fix_.sugg = speller_.suggest(spell_form(s, {d.col0, d.col1}));
    fix_.pick = 0;
    fix_field(fix_.sugg.empty() ? fix_.word : fix_.sugg[0]);
    editor_.select_range({d.line, d.col0}, {d.line, d.col1});
}

// Puts text in the fix field, selected, so typing replaces it.
void App::fix_field(const std::string& text) {
    std::snprintf(bar_buf_, sizeof bar_buf_, "%s", text.c_str());
    bar_set_ = text;  // for when the field is active (see bar_input_callback)
    bar_set_pending_ = true;
    bar_focus_ = true;
    bar_select_pending_ = 1;
}

// On to the next misspelt word from `from`, once the spelling has caught up
// with the edit just made (see draw_fix_bar).
void App::fix_next(TextPos from) {
    fix_from_ = from;
    fix_advance_ = true;
}

void App::add_to_dictionary(const std::string& word) {
    std::string text, err;
    read_file(words_path_, text);
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += word + "\n";
    std::error_code ec;
    fs::create_directories(words_path_.parent_path(), ec);
    if (!atomic_write(words_path_, text, &err)) { status("couldn't save words.txt: " + err, true); return; }
    words_mtime_ = mtime_of(words_path_);  // already applied below: no reload
    speller_.add_word(word);
    status("added \"" + word + "\" to your dictionary");
}

// Steps through the open document's problems. From a note that has none, the
// config's are next door.
void App::goto_problem(int dir) {
    Jump j(*this, History::Problem);
    const Diagnostic* to = nullptr;
    // A note's style slips are stepped through with its problems.
    std::vector<Diagnostic> marks = problems_;
    add_lint_marks(marks, -1, {-1, 0});
    std::stable_sort(marks.begin(), marks.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col0 < b.col0;
    });
    if (marks.empty()) {
        bool config_has = std::any_of(config_problems_.begin(), config_problems_.end(), [](const ConfigIssue& is) { return is.line >= 0; });
        if (is_config() || !config_has) { status(is_config() ? "no problems in the config" : "no problems in this note"); return; }
        open_config();
        if (!is_config() || problems_.empty()) return;
        to = dir > 0 ? &problems_.front() : &problems_.back();  // just opened: the first (or last) one
    } else {
        to = &step_from(marks, editor_.cursor(), editor_.anchor(), dir);
    }
    // The part it's about comes selected, ready to retype.
    TextPos s{to->line, std::max(0, to->col0)}, e{to->line, to->col0 >= 0 ? to->col1 : 0};
    editor_.select_range(s, e);
}

// Fix spelling (see fix_spelling).

void App::draw_fix_bar(ImVec2 min, ImVec2 max, float s, float pad, float ty) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto W = [&](const std::string& t) { return mono_->CalcTextSizeA(s, FLT_MAX, 0, t.c_str()).x; };
    ImGuiIO& io = ImGui::GetIO();
    const auto& lines = editor_.lines();
    float x = min.x + pad;
    dl->AddText(mono_, s, ImVec2(x, ty), T.dim, "fix");
    x += W("fix") + s * 0.5f;

    // After a fix, a skip or an add: the next word, once the spelling marks
    // are current. Forward to the end, then from the top back to where the
    // pass began.
    if (fix_advance_) {
        if (!speller_.ready() || spell_version_ != editor_.version() || spell_gen_ != speller_.generation() || spell_pending_) return;
        fix_advance_ = false;
        int next = fix_pass_next(spelling_, fix_from_, fix_origin_, fix_wrapped_);
        if (next < 0) {
            close_bar();
            status(spelling_.empty() ? "no misspelt words left" : "back where you started");
            return;
        }
        fix_load(spelling_[next]);
    }

    // The word must still be where it was (an undo, a reload...).
    if (fix_.line >= (int)lines.size() || fix_.col1 > (int)lines[fix_.line].size() ||
        lines[fix_.line].compare(fix_.col0, fix_.col1 - fix_.col0, fix_.word) != 0) {
        close_bar();
        return;
    }

    // What the keys (and clicks) do.
    int n = (int)fix_.sugg.size();
    auto fix_with = [&](const std::string& v) {
        TextPos after{fix_.line, fix_.col1};
        if (!v.empty() && v != fix_.word) {
            editor_.replace_range({fix_.line, fix_.col0}, {fix_.line, fix_.col1}, v);
            // The pass goes on after the fix; where it began moves with the text.
            int old_len = (int)fix_.word.size(), new_len = (int)v.size();
            after = after_replace(after, fix_.line, fix_.col0, old_len, new_len);
            fix_origin_ = after_replace(fix_origin_, fix_.line, fix_.col0, old_len, new_len);
        }
        fix_next(after);
    };
    auto skip = [&] { fix_next({fix_.line, fix_.col1}); };
    auto add = [&] {
        add_to_dictionary(spell_form(lines[fix_.line], {fix_.col0, fix_.col1}));
        fix_next({fix_.line, fix_.col1});
    };
    auto guess = [&](int dir) {
        fix_.pick = (fix_.pick + (dir > 0 ? 1 : n - 1)) % n;
        fix_field(fix_.sugg[fix_.pick]);
    };

    // No key repeat: holding enter mustn't race through fixes.
    bool enter = enter_pressed(false);
    if (enter) {
        if (io.KeyCtrl) add();
        else if (io.KeyShift) skip();
        else fix_with(trim(bar_buf_));
        return;
    }
    if (n && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) guess(io.KeyShift ? -1 : 1);

    // Anything that names an action can be clicked: it lights up under the
    // pointer, and the click runs once the bar is drawn.
    std::function<void()> clicked;
    auto hot = [&](float x0, float x1) {
        ImVec2 m = io.MousePos;
        bool over = m.x >= x0 && m.x < x1 && m.y >= min.y && m.y < max.y;
        if (over) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return 2;
        }
        return over ? 1 : 0;
    };

    // Left: the field, wide enough for the longest suggestion.
    float widest = W(fix_.word);
    for (auto& g : fix_.sugg) widest = std::max(widest, W(g));
    float field_w = std::clamp(widest + s * 2, 10 * s, (max.x - min.x) * 0.3f);
    bar_input("##fix", bar_buf_, sizeof bar_buf_, 1, x, ty, field_w);
    x += field_w + s * 1.6f;

    // Right: which suggestion is in the field, and the keys (while there's room).
    float rx = max.x - pad;
    std::string v = trim(bar_buf_);
    std::string count = !n ? "no suggestions" : v == fix_.sugg[fix_.pick] ? std::to_string(fix_.pick + 1) + "/" + std::to_string(n) : "your own";
    const char* sep = " \xC2\xB7 ";  // " · "
    std::vector<std::pair<std::string, std::function<void()>>> keys;
    if (n) keys.push_back({"tab next", [&] { guess(1); }});
    keys.push_back({"shift+enter skip", skip});
    keys.push_back({"ctrl+enter add", add});
    float keys_w = 0;
    for (auto& [label, act] : keys) keys_w += W(label) + (keys_w > 0 ? W(sep) : 0);
    if (rx - keys_w - s * 1.6f - W(count) > x + s * 8) {
        float kx = rx - keys_w;
        ImU32 quiet = T.dim;
        for (size_t i = 0; i < keys.size(); i++) {
            if (i) {
                dl->AddText(mono_, s, ImVec2(kx, ty), quiet, sep);
                kx += W(sep);
            }
            int h = hot(kx, kx + W(keys[i].first));
            if (h == 2) clicked = keys[i].second;
            dl->AddText(mono_, s, ImVec2(kx, ty), h ? T.text : quiet, keys[i].first.c_str());
            kx += W(keys[i].first);
        }
        rx -= keys_w + s * 1.6f;
    }
    rx -= W(count);
    dl->AddText(mono_, s, ImVec2(rx, ty), T.dim, count.c_str());
    rx -= s * 1.6f;

    // Middle: all the suggestions, the one in the field brighter. A click
    // fixes the word with it, as enter would.
    dl->PushClipRect(ImVec2(x, min.y), ImVec2(std::max(x, rx), max.y), true);
    for (int i = 0; i < n; i++) {
        if (i) {
            dl->AddText(mono_, s, ImVec2(x, ty), T.faint, sep);
            x += W(sep);
        }
        const std::string& g = fix_.sugg[i];
        int h = x < rx ? hot(x, std::min(x + W(g), rx)) : 0;
        if (h == 2) clicked = [&fix_with, g] { fix_with(g); };
        bool in_field = i == fix_.pick && v == g;
        dl->AddText(mono_, s, ImVec2(x, ty), h ? T.bright : in_field ? T.text : T.dim, g.c_str());
        x += W(g);
    }
    dl->PopClipRect();

    if (clicked) clicked();
}
