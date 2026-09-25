#include "lint.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "config.h"
#include "spell.h"
#include "util.h"

const std::vector<LintRule>& lint_rules() {
    static const std::vector<LintRule> rules = {
        {"double_space", "two or more spaces between words"},
        {"trailing_space", "spaces or tabs at the end of a line; two spaces after text are a line break and stay"},
        {"blank_lines", "more than one blank line in a row"},
        {"space_before_punct", "a space before , . ; : ! or ?"},
        {"final_newline", "no newline at the end of the note"},
        {"repeated_word", "the same word twice in a row"},
        {"space_after_punct", "no space after . ! or ? between sentences"},
        {"unclosed", "a ( [ or quote left open in its paragraph; no automatic fix"},
        {"heading_blank", "no blank line around a heading"},
        {"fence_blank", "no blank line around a code block"},
        {"list_marker", "- * and + mixed in one list"},
        {"list_indent", "uneven indents in one list"},
    };
    return rules;
}

std::string check_lint_rule(const std::string& id) {
    std::vector<std::string> ids;
    for (auto& r : lint_rules()) {
        if (id == r.id) return {};
        ids.push_back(r.id);
    }
    std::string near = suggest(lower(id), ids);
    return near.empty() ? "no style rule with this name" : "no style rule with this name; did you mean " + near + "?";
}

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t'; }

bool blank(const std::string& s) {
    return std::all_of(s.begin(), s.end(), is_ws);
}

bool any(const std::vector<char>& v, int a, int b) {
    for (int i = std::max(a, 0); i < b && i < (int)v.size(); i++)
        if (v[i]) return true;
    return false;
}

LintProblem problem(int line, int col0, int col1, const char* rule, std::string msg) {
    LintProblem p;
    p.line = line;
    p.col0 = col0;
    p.col1 = col1;
    p.rule = rule;
    p.msg = std::move(msg);
    return p;
}

LintProblem fixed(LintProblem p, TextPos a, TextPos b, std::string text) {
    p.fixable = true;
    p.fix = {a, b, std::move(text)};
    return p;
}

// Columns up to byte `end`, a tab to the next multiple of 4.
int columns(const std::string& s, int end) {
    int c = 0;
    for (int k = 0; k < end && k < (int)s.size(); k++) c = s[k] == '\t' ? (c / 4 + 1) * 4 : c + 1;
    return c;
}

// Words people say twice on purpose ("had had", "bye bye").
bool doubled_on_purpose(const std::string& w) {
    static const char* const ok[] = {"had", "that", "bye", "ha", "no", "so", "very", "really", "blah", "bla", "yeah", "hey"};
    return std::any_of(std::begin(ok), std::end(ok), [&](const char* o) { return w == o; });
}

// A smiley's mouth, not a bracket: ":(", ";)", "=)", ":-(".
bool smiley(const std::string& s, int i) {
    if (i > 0 && std::strchr(":;=", s[i - 1])) return true;
    return i > 1 && s[i - 1] == '-' && std::strchr(":;", s[i - 2]);
}

const char* const kOpen[3] = {"(", "[", "\xE2\x80\x9C"};  // ( [ “
const char* const kClose[3] = {")", "]", "\xE2\x80\x9D"};  // ) ] ”

int bracket_at(const std::string& s, int i, const char* const (&set)[3]) {
    for (int k = 0; k < 3; k++)
        if (s.compare(i, std::strlen(set[k]), set[k]) == 0) return k;
    return -1;
}

// One line's own problems, whatever is around it (see LintLine).
void analyze(const std::string& s, LintLine& L) {
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    md_analyze(s, false, L.info, st, &links);
    const int n = (int)s.size();
    const LineKind kind = L.info.kind;
    auto& own = L.own;

    // Trailing whitespace. Two or more spaces after text are markdown's line
    // break, except on a heading, which can't have one.
    int e = n;
    while (e > 0 && is_ws(s[e - 1])) e--;
    if (e < n) {
        bool tab = s.find('\t', e) != std::string::npos;
        if (e == 0) {
            own.push_back(fixed(problem(0, 0, n, "trailing_space", "spaces on a blank line"), {0, 0}, {0, n}, ""));
        } else if (tab || n - e < 2 || kind == LineKind::Heading) {
            own.push_back(fixed(problem(0, e, n, "trailing_space", tab ? "trailing tab" : "trailing space"), {0, e}, {0, n}, ""));
        }
    }
    if (kind != LineKind::Para && kind != LineKind::Heading) return;

    // What's never touched: code, urls, [[links]], #tags and html tags. And
    // for punctuation, markdown's own symbols too.
    std::vector<char> hard(n, 0), mark(n, 0);
    for (int i = 0; i < n; i++) hard[i] = (st[i] & (S_Code | S_CodeBlock | S_Url | S_Tag)) != 0;
    for (auto& l : links) {
        bool bare_url = !l.wiki && !l.tag && s[l.begin] != '[';
        if (l.wiki || bare_url)
            for (int k = l.begin; k < l.end && k < n; k++) hard[k] = 1;
    }
    for (int i = 0; i + 1 < n; i++) {
        if (s[i] != '<' || !(std::isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) continue;
        size_t close = s.find('>', i);
        if (close == std::string::npos) break;
        for (int k = i; k <= (int)close; k++) hard[k] = 1;
        i = (int)close;
    }
    for (int i = 0; i < n; i++) mark[i] = hard[i] || (st[i] & (S_Marker | S_TaskBox | S_ListMark));

    int lead = 0;
    while (lead < n && is_ws(s[lead])) lead++;
    // A table row lines its cells up with spaces, and " :" is its alignment.
    bool table = lead < n && s[lead] == '|';

    // Double spaces between words (indentation is leading, not between).
    for (int i = lead; i < e && !table;) {
        if (s[i] != ' ') { i++; continue; }
        int a = i;
        while (i < e && s[i] == ' ') i++;
        if (i - a >= 2 && !any(hard, a, i))
            own.push_back(fixed(problem(0, a, i, "double_space", "double space"), {0, a}, {0, i}, " "));
    }

    // A space before punctuation that ends something (not " .md", " ...").
    for (int i = lead; i < e && !table; i++) {
        char c = s[i];
        if (!std::strchr(",.;:!?", c) || mark[i]) continue;
        int j = i;
        while (j > lead && s[j - 1] == ' ') j--;
        if (j == i || any(hard, j, i) || (st[j - 1] & S_ListMark)) continue;
        bool ends = i + 1 >= n || is_ws(s[i + 1]) || std::strchr("\"'*_", s[i + 1]);
        if (!ends) continue;
        own.push_back(fixed(problem(0, j, i + 1, "space_before_punct", std::string("space before \"") + c + "\""), {0, j}, {0, i}, ""));
    }

    // No space between sentences: "end.Next". A capital then a small letter
    // after it, and a small letter before, so 3.14, e.g., U.S.A, notes.md
    // and 10:30 pass; so does anything in a word that reads as code or an
    // address.
    for (int i = lead; i < e; i++) {
        if (!std::strchr(".!?", s[i]) || mark[i]) continue;
        int a = i, b = i;
        while (b < n && s[b] && std::strchr(".!?", s[b])) b++;
        i = b - 1;
        if (a == 0 || !std::islower((unsigned char)s[a - 1])) continue;
        if (b + 1 >= n || !std::isupper((unsigned char)s[b]) || !std::islower((unsigned char)s[b + 1])) continue;
        if (any(mark, a - 1, b + 2)) continue;
        int t0 = a, t1 = b;
        while (t0 > 0 && !is_ws(s[t0 - 1])) t0--;
        while (t1 < n && !is_ws(s[t1])) t1++;
        bool codeish = false;
        for (int k = t0; k < t1; k++) codeish |= std::strchr("/\\@_=<>{}$|~^`", s[k]) != nullptr;
        if (codeish) continue;
        own.push_back(fixed(problem(0, a, b, "space_after_punct", "no space after \"" + s.substr(a, b - a) + "\""), {0, b}, {0, b}, " "));
    }

    // A word said twice: "the the". Only across one space, any case.
    std::vector<WordSpan> words = spell_words(s);
    for (size_t k = 1; k < words.size(); k++) {
        WordSpan p = words[k - 1], w = words[k];
        if (w.begin != p.end + 1 || s[p.end] != ' ' || w.end - w.begin != p.end - p.begin) continue;
        std::string a = lower(s.substr(p.begin, p.end - p.begin)), b = lower(s.substr(w.begin, w.end - w.begin));
        if (a != b || doubled_on_purpose(a)) continue;
        own.push_back(fixed(problem(0, w.begin, w.end, "repeated_word", "repeated word"), {0, p.end}, {0, w.end}, ""));
    }

    // Brackets and quotes: matched on the line where they can be; what's
    // left is settled by the paragraph (see lint).
    std::vector<int> stack[3];
    for (int i = lead; i < n; i++) {
        if (mark[i]) continue;
        if (int k = bracket_at(s, i, kOpen); k >= 0) {
            if (k == 0 && smiley(s, i)) continue;
            stack[k].push_back(i);
        } else if (int k2 = bracket_at(s, i, kClose); k2 >= 0) {
            if (k2 == 0 && smiley(s, i)) continue;
            if (!stack[k2].empty()) stack[k2].pop_back();
            else L.closes[k2]++;
        } else if (s[i] == '"' && !(i > 0 && std::isdigit((unsigned char)s[i - 1]))) {  // 12" is inches
            L.quotes.push_back(i);
        }
    }
    for (int k = 0; k < 3; k++)
        for (int c : stack[k]) L.opens.push_back({c, k});
    std::sort(L.opens.begin(), L.opens.end());
}

// The line that closes the front matter at the top of a note, or -1.
int front_matter_end(const std::vector<std::string>& lines) {
    if (lines.empty() || trim(lines[0]) != "---") return -1;
    for (int k = 1; k < (int)lines.size(); k++) {
        std::string t = trim(lines[k]);
        if (t == "---" || t == "...") return k;
    }
    return -1;  // unclosed, it's a rule
}

}  // namespace

std::vector<LintProblem> lint(const std::vector<std::string>& lines, const std::vector<std::string>& off, LintCache* cache) {
    const int n = (int)lines.size();
    std::vector<LintProblem> out;
    auto add = [&](LintProblem p) { out.push_back(std::move(p)); };

    // Front matter and fenced code are skipped; the fences' own blank lines
    // are checked on the way.
    const int fm = front_matter_end(lines);
    std::vector<char> skip(n, 0);
    for (int i = 0; i <= fm; i++) skip[i] = 1;
    auto is_blank = [&](int i) { return i >= 0 && i < n && !skip[i] && blank(lines[i]); };
    bool in_code = false;
    for (int i = fm + 1; i < n; i++) {
        bool fence = md_is_fence(lines[i]);
        skip[i] = in_code || fence;
        if (!fence) continue;
        bool opening = !in_code;
        in_code = !in_code;
        if (is_ws(lines[i][0])) continue;  // indented: inside a list item
        int f1 = 0;
        while (f1 < (int)lines[i].size() && lines[i][f1] == lines[i][0]) f1++;
        if (opening && i - 1 > fm && !blank(lines[i - 1]))
            add(fixed(problem(i, 0, f1, "fence_blank", "no blank line before code block"), {i, 0}, {i, 0}, "\n"));
        if (!opening && i + 1 < n && !blank(lines[i + 1]))
            add(fixed(problem(i, 0, f1, "fence_blank", "no blank line after code block"), {i + 1, 0}, {i + 1, 0}, "\n"));
    }

    // Each line's own look, from the cache when it's been seen.
    LintCache scratch;
    LintCache& C = cache ? *cache : scratch;
    if (C.lines.size() > 20000) C.lines.clear();  // a long session's typing
    std::vector<const LintLine*> F(n, nullptr);
    for (int i = 0; i < n; i++) {
        if (skip[i]) continue;
        auto [it, fresh] = C.lines.try_emplace(lines[i]);
        if (fresh) analyze(lines[i], it->second);
        F[i] = &it->second;
        for (const LintProblem& p : it->second.own) {
            LintProblem q = p;
            q.line = q.fix.a.line = q.fix.b.line = i;
            add(std::move(q));
        }
    }

    // Runs of blank lines. At the end of the note the one left is the final
    // newline.
    for (int i = 0; i < n;) {
        if (!is_blank(i)) { i++; continue; }
        int a = i;
        while (is_blank(i)) i++;
        int b = i - 1;
        if (b > a)
            add(fixed(problem(a + 1, 0, 0, "blank_lines", b - a > 1 ? "extra blank lines" : "extra blank line"),
                      {a, (int)lines[a].size()}, {b, (int)lines[b].size()}, ""));
    }

    if (n > 0 && !skip[n - 1] && !blank(lines[n - 1])) {
        int len = (int)lines[n - 1].size();
        LintProblem p = fixed(problem(n - 1, len, len, "final_newline", "no newline at the end"), {n - 1, len}, {n - 1, len}, "\n");
        p.fix.before = true;
        add(std::move(p));
    }

    // Headings: a blank line on either side (a #tag is never a heading: it
    // has no space after its #).
    for (int i = 0; i < n; i++) {
        if (!F[i] || F[i]->info.kind != LineKind::Heading) continue;
        int h0 = 0;
        while (h0 < (int)lines[i].size() && is_ws(lines[i][h0])) h0++;
        int h1 = h0 + F[i]->info.level;
        if (i - 1 > fm && !blank(lines[i - 1]))
            add(fixed(problem(i, h0, h1, "heading_blank", "no blank line before heading"), {i, 0}, {i, 0}, "\n"));
        if (i + 1 < n && !blank(lines[i + 1]))
            add(fixed(problem(i, h0, h1, "heading_blank", "no blank line after heading"), {i + 1, 0}, {i + 1, 0}, "\n"));
    }

    // Lists: one marker per level, and indents in even steps (the first
    // nested item's). A list runs on over blank lines while items or
    // indented lines follow.
    struct List {
        bool on = false, gap = false, tabs = false;
        int base = 0, step = 0, prev = 0;
        std::vector<char> marker;  // by level; 0 for none yet
    } list;
    for (int i = 0; i < n; i++) {
        if (!F[i]) { list.on = false; continue; }
        const LineInfo& info = F[i]->info;
        const std::string& s = lines[i];
        if (info.kind == LineKind::Blank) { list.gap = list.on; continue; }
        bool item = info.list && !info.quote && info.kind == LineKind::Para;
        if (!item) {
            bool more = list.on && info.kind == LineKind::Para && !info.quote && (!list.gap || is_ws(s[0]));
            if (!more) list.on = false;
            list.gap = false;
            continue;
        }
        list.gap = false;
        int mb = info.marker_begin, w = columns(s, mb);
        if (!list.on) {
            list = {};
            list.on = true;
            list.base = w;
        }
        int rel = std::max(0, w - list.base), level = 0;
        if (rel > 0) {
            if (!list.step) {
                list.step = rel;
                list.tabs = s.substr(0, mb).find('\t') != std::string::npos;
            }
            level = std::min((2 * rel + list.step - 1) / (2 * list.step), list.prev + 1);  // nearest, half down
            if (level * list.step != rel) {
                std::string indent = list.tabs && list.base == 0 ? std::string(level, '\t') : std::string(list.base + level * list.step, ' ');
                std::string step = list.tabs ? "a tab" : std::to_string(list.step);
                add(fixed(problem(i, 0, mb, "list_indent", "uneven list indent; this list steps by " + step), {i, 0}, {i, mb}, indent));
            }
        }
        list.prev = level;
        list.marker.resize(level + 1, 0);  // deeper lists under the one before end here
        char& m = list.marker[level];
        if (info.ordered) {
            m = 0;
        } else if (!m) {
            m = s[mb];
        } else if (s[mb] != m) {
            add(fixed(problem(i, mb, mb + 1, "list_marker", std::string(1, s[mb]) + " in a list of " + m),
                      {i, mb}, {i, mb + 1}, std::string(1, m)));
        }
    }

    // Brackets and quotes left open, settled a paragraph at a time: a
    // closer on a later line of it closes one left open before.
    std::vector<std::pair<int, int>> open[3];
    int q_line = -1, q_col = 0;
    // The mark takes in the word after the bracket too: a lone "(" is too
    // thin to see a mark under.
    auto word_end = [&](int l, int c) {
        int e = c;
        while (e < (int)lines[l].size() && !is_ws(lines[l][e]) && e - c < 24) e++;
        while (e < (int)lines[l].size() && ((unsigned char)lines[l][e] & 0xC0) == 0x80) e++;  // a whole character
        return e;
    };
    auto settle = [&] {
        for (int k = 0; k < 3; k++) {
            for (auto [l, c] : open[k])
                add(problem(l, c, word_end(l, c + (int)std::strlen(kOpen[k])), "unclosed", std::string(kOpen[k]) + " not closed; fix by hand"));
            open[k].clear();
        }
        if (q_line >= 0) add(problem(q_line, q_col, word_end(q_line, q_col + 1), "unclosed", "\" not closed; fix by hand"));
        q_line = -1;
    };
    for (int i = 0; i < n; i++) {
        LineKind kind = F[i] ? F[i]->info.kind : LineKind::Blank;
        if (kind != LineKind::Para && kind != LineKind::Heading) { settle(); continue; }
        if (kind == LineKind::Heading || F[i]->info.list) settle();
        for (int k = 0; k < 3; k++)
            for (int c = 0; c < F[i]->closes[k] && !open[k].empty(); c++) open[k].pop_back();
        for (auto [c, k] : F[i]->opens) open[k].push_back({i, c});
        for (int q : F[i]->quotes) {
            if (q_line >= 0) q_line = -1;
            else q_line = i, q_col = q;
        }
        if (kind == LineKind::Heading) settle();
    }
    settle();

    // Rules turned off go; a double space before punctuation is the other
    // rule's (one mark, one fix).
    auto is_off = [&](const char* id) { return std::find(off.begin(), off.end(), id) != off.end(); };
    out.erase(std::remove_if(out.begin(), out.end(), [&](const LintProblem& p) { return is_off(p.rule); }), out.end());
    std::stable_sort(out.begin(), out.end(), [](const LintProblem& a, const LintProblem& b) {
        return a.line != b.line ? a.line < b.line : a.col0 < b.col0;
    });
    for (size_t k = 0; k + 1 < out.size(); k++)
        if (std::strcmp(out[k].rule, "double_space") == 0 && out[k + 1].line == out[k].line && out[k + 1].col0 == out[k].col0 &&
            std::strcmp(out[k + 1].rule, "space_before_punct") == 0)
            out.erase(out.begin() + (long)k--);
    return out;
}

int lint_at(const std::vector<LintProblem>& problems, TextPos p) {
    for (int i = 0; i < (int)problems.size(); i++)
        if (problems[i].line == p.line && problems[i].col0 <= p.col && p.col <= problems[i].col1) return i;
    return -1;
}

TextPos after_fix(TextPos p, const LintFix& f) {
    if (p < f.a) return p;
    if (f.before && f.a == f.b && p == f.a) return p;
    // Where the new text ends.
    int nl = 0;
    size_t last = std::string::npos;
    for (size_t k = 0; k < f.text.size(); k++)
        if (f.text[k] == '\n') nl++, last = k;
    TextPos e = nl ? TextPos{f.a.line + nl, (int)(f.text.size() - last - 1)} : TextPos{f.a.line, f.a.col + (int)f.text.size()};
    if (p < f.b) return e;
    if (p.line == f.b.line) return {e.line, e.col + p.col - f.b.col};
    return {p.line + e.line - f.b.line, p.col};
}

// Makes one fix in the lines; false if it no longer fits them.
static bool apply_one(std::vector<std::string>& lines, const LintFix& f) {
    auto fits = [&](TextPos p) { return p.line >= 0 && p.line < (int)lines.size() && p.col >= 0 && p.col <= (int)lines[p.line].size(); };
    if (!fits(f.a) || !fits(f.b) || f.b < f.a) return false;
    std::string head = lines[f.a.line].substr(0, f.a.col), tail = lines[f.b.line].substr(f.b.col);
    lines.erase(lines.begin() + f.a.line + 1, lines.begin() + f.b.line + 1);
    std::vector<std::string> parts = split_lines(f.text);
    lines[f.a.line] = head + parts[0];
    lines.insert(lines.begin() + f.a.line + 1, parts.begin() + 1, parts.end());
    lines[f.a.line + parts.size() - 1] += tail;
    return true;
}

int apply_fixes(std::vector<std::string>& lines, const std::vector<LintProblem>& problems, TextPos* cur, TextPos* anchor) {
    std::vector<const LintFix*> fixes;
    for (auto& p : problems)
        if (p.fixable) fixes.push_back(&p.fix);
    std::stable_sort(fixes.begin(), fixes.end(), [](const LintFix* x, const LintFix* y) {
        return x->a != y->a ? x->a < y->a : x->b < y->b;
    });
    // One fix per stretch of text: the same fix found twice (the blank line
    // between two headings) goes in once, and one overlapping another waits
    // for the next round.
    std::vector<const LintFix*> keep;
    for (auto* f : fixes) {
        if (!keep.empty()) {
            const LintFix& k = *keep.back();
            if (f->a == k.a && f->b == k.b && f->text == k.text) continue;
            if (f->a < k.b) continue;
        }
        keep.push_back(f);
    }
    // From the last, so the ones before keep their places.
    int done = 0;
    for (auto it = keep.rbegin(); it != keep.rend(); ++it) {
        if (!apply_one(lines, **it)) continue;
        done++;
        if (cur) *cur = after_fix(*cur, **it);
        if (anchor) *anchor = after_fix(*anchor, **it);
    }
    return done;
}

int lint_format(std::vector<std::string>& lines, const std::vector<std::string>& off, TextPos* cur, TextPos* anchor) {
    int total = 0;
    for (int round = 0; round < 10; round++) {
        int done = apply_fixes(lines, lint(lines, off), cur, anchor);
        if (!done) break;
        total += done;
    }
    return total;
}
