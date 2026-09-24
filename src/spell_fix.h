#pragma once
// The decisions behind fixing spelling, kept apart from the app so they can
// be tested: which word a fix is about, where a fix pass goes next, and
// where a position lands after a word changes length. `marks` are spelling
// marks in text order.
#include <string>
#include <vector>

#include "editor.h"

// The misspelt word the caret is in (or at either end of); -1 if none.
int misspelling_at(const std::vector<Diagnostic>& marks, TextPos caret);

// That, or failing it the nearest misspelt word before the caret in its
// paragraph (lines up to the blank one above; from a blank line, the
// paragraph just above it). -1 if none.
int misspelling_near(const std::vector<Diagnostic>& marks, const std::vector<std::string>& lines, TextPos caret);

// The next word of a fix pass that began at `origin`: the first at or after
// `from`, up to the end; then, once `wrapped`, from the top up to (not
// including) `origin`. -1: the pass is over.
int fix_pass_next(const std::vector<Diagnostic>& marks, TextPos from, TextPos origin, bool& wrapped);

// Where `p` ends up when the `old_len` bytes at (line, col0) become
// `new_len`: moved along if it was after them, at their new end if it was
// inside, where it was otherwise.
TextPos after_replace(TextPos p, int line, int col0, int old_len, int new_len);
