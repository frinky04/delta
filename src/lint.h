#pragma once
// Style slips in a note, the way a code formatter finds them: stray
// whitespace, small prose slips and uneven markdown. One set of rules; each
// marks its problems and knows their fix (or says there's none). Code,
// [[links]], urls, #tags and the front matter are never touched. Pure text
// work: the app marks the problems and applies the fixes.
#include <string>
#include <unordered_map>
#include <vector>

#include "editor.h"
#include "markdown.h"

// A rule: its id (for lint_off) and what it looks for.
struct LintRule {
    const char* id;
    const char* desc;
};
const std::vector<LintRule>& lint_rules();
// Why `id` can't be turned off, being no rule's ("" if it's one), for the
// config's lint_off.
std::string check_lint_rule(const std::string& id);

// An edit: the text from a up to b becomes `text` (which may hold '\n').
// A caret right where it inserts moves past the new text, unless `before`
// (the final newline: typing at the end of a note, you stay there).
struct LintFix {
    TextPos a, b;
    std::string text;
    bool before = false;
};

// A problem: the bytes marked (col0 == col1 marks a point), which rule found
// it, what it says, and its fix if it has one.
struct LintProblem {
    int line = 0, col0 = 0, col1 = 0;
    const char* rule = "";
    std::string msg;
    bool fixable = false;
    LintFix fix;
};

// What one line says about itself, whatever lines are around it: its shape,
// its own problems (on line 0), and the brackets it leaves open or closes,
// which a paragraph settles. Kept by the line's text, so typing only looks
// at the line that changed.
struct LintLine {
    LineInfo info;
    std::vector<LintProblem> own;
    std::vector<std::pair<int, int>> opens;  // (col, kind): ( [ and curly quotes left open
    int closes[3] = {0, 0, 0};               // closers with nothing to close, by kind
    std::vector<int> quotes;                 // straight double quotes
};
struct LintCache {
    std::unordered_map<std::string, LintLine> lines;
};

// The problems in these lines, in text order, leaving out the rules in
// `off` (ids).
std::vector<LintProblem> lint(const std::vector<std::string>& lines, const std::vector<std::string>& off = {},
                              LintCache* cache = nullptr);

// The problem marked at a place (the caret in it or at either end); -1 if none.
int lint_at(const std::vector<LintProblem>& problems, TextPos p);

// Where `p` ends up after a fix: moved along if it was after the text
// changed, at the end of the new text if it was inside.
TextPos after_fix(TextPos p, const LintFix& f);

// Applies the fixes of these problems that don't overlap one another; `cur`
// and `anchor`, if given, move with the text. Returns how many.
int apply_fixes(std::vector<std::string>& lines, const std::vector<LintProblem>& problems, TextPos* cur = nullptr,
                TextPos* anchor = nullptr);

// Every fix, again until there's nothing left to fix (one fix can make room
// for another), so formatting twice is formatting once. Returns how many.
int lint_format(std::vector<std::string>& lines, const std::vector<std::string>& off = {}, TextPos* cur = nullptr,
                TextPos* anchor = nullptr);
