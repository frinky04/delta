#include "spell_fix.h"

#include "util.h"

static TextPos start(const Diagnostic& d) { return {d.line, d.col0}; }

int misspelling_at(const std::vector<Diagnostic>& marks, TextPos caret) {
    for (int i = 0; i < (int)marks.size(); i++)
        if (marks[i].line == caret.line && marks[i].col0 <= caret.col && caret.col <= marks[i].col1) return i;
    return -1;
}

int misspelling_near(const std::vector<Diagnostic>& marks, const std::vector<std::string>& lines, TextPos caret) {
    if (int i = misspelling_at(marks, caret); i >= 0) return i;
    int top = caret.line;
    while (top > 0 && top - 1 < (int)lines.size() && !trim(lines[top - 1]).empty()) top--;
    for (int i = (int)marks.size() - 1; i >= 0; i--)
        if (marks[i].line >= top && start(marks[i]) < caret) return i;
    return -1;
}

int fix_pass_next(const std::vector<Diagnostic>& marks, TextPos from, TextPos origin, bool& wrapped) {
    for (int i = 0; i < (int)marks.size(); i++)
        if (!(start(marks[i]) < from) && (!wrapped || start(marks[i]) < origin)) return i;
    if (wrapped) return -1;
    wrapped = true;
    for (int i = 0; i < (int)marks.size(); i++)
        if (start(marks[i]) < origin) return i;
    return -1;
}

TextPos after_replace(TextPos p, int line, int col0, int old_len, int new_len) {
    if (p.line != line) return p;
    if (p.col >= col0 + old_len) p.col += new_len - old_len;
    else if (p.col > col0) p.col = col0 + new_len;
    return p;
}
