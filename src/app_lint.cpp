// Style slips in the note (double spaces, a word said twice, an uneven
// list): found as the note changes, marked quieter than spelling, echoed in
// the status line, fixed one at a time ("fix style slip", on alt+. like the
// quick fix, where the caret is on one) or all at once by
// "format note". The rules and their fixes are lint.h's.
#include "app.h"
#include "spell_fix.h"
#include "util.h"

// The open note's slips, for the text as it is now. Only lines that changed
// are looked at again (see LintCache).
void App::check_lint() {
    if (editor_.version() == lint_version_ && lint_on_ == (is_note() && cfg_.lint) && lint_off_ == cfg_.lint_off) return;
    lint_version_ = editor_.version();
    lint_on_ = is_note() && cfg_.lint;
    lint_off_ = cfg_.lint_off;
    if (lint_on_) lint_ = lint(editor_.lines(), split_list(cfg_.lint_off), &lint_cache_);
    else lint_.clear();
    diag_gen_++;
}

// A slip the caret has sat at since the edit that made it (a space just
// typed at the end of a line) is being written, not a slip: it waits for
// the caret to move on. Returns where (line -1: none).
TextPos App::lint_held() const {
    TextPos c = editor_.cursor();
    if (spell_hold_.line < 0 || c != spell_hold_ || lint_at(lint_, c) < 0) return {-1, 0};
    return c;
}

// The slips as marks, but for those on line `hold` (being typed on) and the
// one at `held`.
void App::add_lint_marks(std::vector<Diagnostic>& out, int hold, TextPos held) const {
    for (auto& p : lint_) {
        if (p.line == hold || (p.line == held.line && p.col0 <= held.col && held.col <= p.col1)) continue;
        Diagnostic d;
        d.line = p.line;
        d.col0 = p.col0;
        d.col1 = p.col1;
        d.msg = p.msg;
        d.kind = Diagnostic::Kind::Lint;
        out.push_back(std::move(d));
    }
}

// The slip the caret is on, as the echo shows it (not one being written).
const LintProblem* App::lint_at_caret() const {
    if (lint_held().line >= 0) return nullptr;
    int i = lint_at(lint_, editor_.cursor());
    return i >= 0 ? &lint_[i] : nullptr;
}

// Whether the caret is on a slip (one just typed too), where "fix style
// slip" has the keys. A misspelt word there keeps them for the quick fix.
bool App::slip_at_caret() const {
    TextPos c = editor_.cursor();
    return editing() && is_note() && lint_at(lint_, c) >= 0 && misspelling_at(spelling_, c) < 0;
}

// The slip at the caret fixed: its fix goes in, and the caret stays with
// the text it was by. One without a fix says so.
void App::fix_slip() {
    if (!need_note() || editor_.reader()) return;
    check_lint();  // for the text as it is now, not last frame's
    TextPos c = editor_.cursor();
    int i = lint_at(lint_, c);
    if (i < 0) { status(cfg_.lint ? "no style slip here" : "style marks are off"); return; }
    const LintProblem& p = lint_[i];
    if (!p.fixable) { status(p.msg); return; }
    LintFix f = p.fix;
    editor_.replace_range(f.a, f.b, f.text);
    editor_.jump_to(after_fix(c, f));
}

// Every fix in the note as one edit (one undo puts it all back), the caret
// and selection kept with the text they were on. `on_save`: part of "save
// note now", which says the rest.
void App::format_note(bool on_save) {
    if (!on_save && !need_note()) return;
    std::vector<std::string> lines = editor_.lines();
    TextPos cur = editor_.cursor(), anchor = editor_.anchor();
    std::vector<std::string> off = split_list(cfg_.lint_off);
    int fixed = lint_format(lines, off, &cur, &anchor);
    if (fixed) editor_.rewrite(lines, cur, anchor);
    if (on_save) return;
    int left = (int)lint(lines, off).size();  // those with no fix
    if (!fixed && !left) status("nothing to format");
    else if (!fixed) status(count_of(left, "slip") + " to fix by hand");
    else status("fixed " + count_of(fixed, "style slip") + (left ? "; " + std::to_string(left) + " to fix by hand" : ""));
}
