#include "markdown.h"

#include <cctype>
#include <cstring>

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static bool is_word(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

float md_heading_scale(int level) {
    // Close to body size, after Obsidian's Minimal theme: headings are set
    // apart by weight and brightness more than by size.
    switch (level) {
        case 1: return 1.125f;
        case 2: return 1.05f;
        case 3: return 1.0f;
        case 4: return 0.9f;
        default: return 0.85f;
    }
}

bool md_is_fence(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && i < 3 && s[i] == ' ') i++;
    return s.compare(i, 3, "```") == 0 || s.compare(i, 3, "~~~") == 0;
}

static bool is_rule(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && i < 3 && s[i] == ' ') i++;
    if (i >= s.size()) return false;
    char c = s[i];
    if (c != '-' && c != '*' && c != '_') return false;
    int count = 0;
    for (; i < s.size(); i++) {
        if (s[i] == c) count++;
        else if (!is_ws(s[i])) return false;
    }
    return count >= 3;
}

static void mark(std::vector<uint16_t>& st, size_t a, size_t b, uint16_t f) {
    for (size_t i = a; i < b && i < st.size(); i++) st[i] |= f;
}

static void inline_parse(const std::string& s, size_t from, std::vector<uint16_t>& st, std::vector<LinkSpan>* links) {
    const size_t n = s.size();
    std::vector<char> used(n, 0);
    auto use = [&](size_t a, size_t b) { for (size_t i = a; i < b && i < n; i++) used[i] = 1; };
    auto any_used = [&](size_t a, size_t b) { for (size_t i = a; i < b && i < n; i++) if (used[i]) return true; return false; };

    // Code spans first: nothing inside them is markdown.
    for (size_t i = from; i < n;) {
        if (s[i] != '`') { i++; continue; }
        size_t k = i;
        while (k < n && s[k] == '`') k++;
        size_t run = k - i, close = std::string::npos;
        for (size_t j = k; j < n;) {
            if (s[j] != '`') { j++; continue; }
            size_t e = j;
            while (e < n && s[e] == '`') e++;
            if (e - j == run) { close = j; break; }
            j = e;
        }
        if (close == std::string::npos) { i = k; continue; }
        mark(st, i, close + run, S_Code);
        mark(st, i, k, S_Marker);
        mark(st, close, close + run, S_Marker);
        use(i, close + run);
        i = close + run;
    }

    // [[wiki links]] and [[target|alias]]
    for (size_t i = from; i + 1 < n; i++) {
        if (used[i] || s[i] != '[' || s[i + 1] != '[') continue;
        size_t close = s.find("]]", i + 2);
        if (close == std::string::npos || close == i + 2 || any_used(i, close + 2)) continue;
        std::string inner = s.substr(i + 2, close - i - 2);
        if (inner.find('[') != std::string::npos) continue;
        mark(st, i, close + 2, S_WikiLink);
        mark(st, i, i + 2, S_Marker);
        mark(st, close, close + 2, S_Marker);
        size_t bar = inner.find('|');
        std::string target = inner;
        if (bar != std::string::npos) {
            mark(st, i + 2, i + 3 + bar, S_Marker | S_Url);
            target = inner.substr(0, bar);
        }
        if (links) links->push_back({(int)i, (int)(close + 2), target, true});
        use(i, close + 2);
        i = close + 1;
    }

    // [text](url)
    for (size_t i = from; i < n; i++) {
        if (used[i] || s[i] != '[') continue;
        size_t j = i + 1;
        while (j < n && s[j] != ']' && s[j] != '[' && !used[j]) j++;
        if (j + 1 >= n || s[j] != ']' || s[j + 1] != '(') continue;
        size_t k = s.find(')', j + 2);
        if (k == std::string::npos || any_used(j, k + 1)) continue;
        mark(st, i + 1, j, S_Link);
        mark(st, i, i + 1, S_Marker | S_Link);
        mark(st, j, k + 1, S_Marker | S_Url);
        if (links) links->push_back({(int)i, (int)(k + 1), s.substr(j + 2, k - j - 2), false});
        used[i] = 1;
        use(j, k + 1);
        i = k;
    }

    // Bare urls.
    for (size_t i = from; i < n; i++) {
        if (used[i]) continue;
        size_t len = s.compare(i, 8, "https://") == 0 ? 8 : s.compare(i, 7, "http://") == 0 ? 7 : 0;
        if (!len || (i > 0 && is_word((unsigned char)s[i - 1]))) continue;
        size_t e = i + len;
        while (e < n && !is_ws(s[e]) && !used[e] && s[e] != '>' && s[e] != ')') e++;
        while (e > i + len && std::strchr(".,;:!?", s[e - 1])) e--;
        if (e == i + len) continue;
        mark(st, i, e, S_Link);
        if (links) links->push_back({(int)i, (int)e, s.substr(i, e - i), false});
        use(i, e);
        i = e;
    }

    // Emphasis. Longer delimiters first so ** isn't eaten as two *s.
    struct Delim { const char* d; size_t len; uint16_t flag; bool intraword_ok; };
    static const Delim delims[] = {
        {"**", 2, S_Bold, true}, {"__", 2, S_Bold, false}, {"~~", 2, S_Strike, true},
        {"==", 2, S_Highlight, true}, {"*", 1, S_Italic, true}, {"_", 1, S_Italic, false},
    };
    for (const Delim& d : delims) {
        auto at = [&](size_t i) { return i + d.len <= n && s.compare(i, d.len, d.d) == 0 && !any_used(i, i + d.len); };
        for (size_t i = from; i + d.len <= n;) {
            size_t a = i + d.len;
            bool open = at(i) && a < n && !is_ws(s[a]) && s[a] != d.d[0] &&
                        (d.intraword_ok || i == 0 || !is_word((unsigned char)s[i - 1]));
            if (!open) { i++; continue; }
            size_t close = std::string::npos;
            for (size_t j = a + 1; j + d.len <= n; j++) {
                if (!at(j) || is_ws(s[j - 1])) continue;
                if (!d.intraword_ok && j + d.len < n && is_word((unsigned char)s[j + d.len])) continue;
                if (j + d.len < n && s[j + d.len] == d.d[0] && d.len == 1) continue;  // part of a longer run
                close = j;
                break;
            }
            if (close == std::string::npos) { i++; continue; }
            mark(st, a, close, d.flag);
            mark(st, i, a, S_Marker | d.flag);
            mark(st, close, close + d.len, S_Marker | d.flag);
            use(i, a);
            use(close, close + d.len);
            i = close + d.len;
        }
    }
}

void md_analyze(const std::string& s, bool in_code, LineInfo& info, std::vector<uint16_t>& st, std::vector<LinkSpan>* links) {
    const size_t n = s.size();
    st.assign(n, 0);
    info = LineInfo{};

    if (md_is_fence(s)) {
        info.kind = LineKind::Fence;
        mark(st, 0, n, S_CodeBlock | S_Marker);
        return;
    }
    if (in_code) {
        info.kind = LineKind::CodeBlock;
        mark(st, 0, n, S_CodeBlock);
        return;
    }

    size_t i = 0;
    while (i < n && is_ws(s[i])) i++;
    if (i == n) { info.kind = LineKind::Blank; return; }
    if (is_rule(s)) {
        info.kind = LineKind::Rule;
        mark(st, 0, n, S_Marker);
        return;
    }

    if (i <= 3 && s[i] == '#') {
        size_t j = i;
        while (j < n && s[j] == '#') j++;
        if (j - i <= 6 && (j == n || s[j] == ' ')) {
            info.kind = LineKind::Heading;
            info.level = (int)(j - i);
            size_t content = j < n ? j + 1 : n;
            mark(st, 0, n, S_Heading);
            mark(st, 0, content, S_Marker);
            info.content_start = info.marker_begin = (int)content;
            inline_parse(s, content, st, links);
            return;
        }
    }

    size_t p = 0;
    if (i <= 3 && s[i] == '>') {
        info.quote = true;
        size_t j = i;
        while (j < n && (s[j] == '>' || s[j] == ' ')) j++;
        mark(st, 0, j, S_Marker | S_QuoteMark);
        mark(st, j, n, S_Quote);
        p = j;
    }

    size_t q = p;
    while (q < n && is_ws(s[q])) q++;
    size_t m = q;
    info.marker_begin = (int)q;
    if (q < n) {
        if ((s[q] == '-' || s[q] == '*' || s[q] == '+') && q + 1 < n && s[q + 1] == ' ') {
            info.list = true;
            mark(st, q, q + 2, S_Marker | S_ListMark);
            m = q + 2;
        } else if (std::isdigit((unsigned char)s[q])) {
            size_t d = q;
            while (d < n && std::isdigit((unsigned char)s[d])) d++;
            if (d - q <= 9 && d < n && (s[d] == '.' || s[d] == ')') && (d + 1 == n || s[d + 1] == ' ')) {
                info.list = info.ordered = true;
                m = d + 1 < n ? d + 2 : d + 1;
                mark(st, q, m, S_ListMark);  // not a marker: numbers stay in reader
            }
        }
        if (info.list && m + 2 < n && s[m] == '[' && (s[m + 1] == ' ' || s[m + 1] == 'x' || s[m + 1] == 'X') &&
            s[m + 2] == ']' && (m + 3 == n || s[m + 3] == ' ')) {
            info.task = true;
            info.done = s[m + 1] != ' ';
            size_t e = m + 3 < n ? m + 4 : m + 3;
            mark(st, m, e, S_Marker | S_TaskBox);
            if (info.done) mark(st, e, n, S_TaskDone);
            m = e;
        }
    }
    info.content_start = (int)(info.list ? m : p);
    inline_parse(s, info.content_start, st, links);
}

void md_reader(const std::string& s, const LineInfo& info, const std::vector<uint16_t>& st,
               std::string& out, std::vector<uint16_t>& ost, std::vector<int>& map) {
    out.clear(); ost.clear(); map.clear();
    auto emit = [&](const char* text, uint16_t style, int src) {
        for (const char* c = text; *c; c++) { out += *c; ost.push_back(style); map.push_back(src); }
    };
    if (info.kind == LineKind::Fence || info.kind == LineKind::Rule) return;
    if (info.kind == LineKind::CodeBlock) {
        out = s; ost = st;
        for (int i = 0; i < (int)s.size(); i++) map.push_back(i);
        return;
    }
    const int n = (int)s.size();
    for (int i = 0; i < n;) {
        uint16_t f = st[i];
        auto run_end = [&](uint16_t flag) { int j = i; while (j < n && (st[j] & flag)) j++; return j; };
        if (f & S_QuoteMark) {
            emit("  ", S_QuoteMark, i);  // bar is drawn over this gap
            i = run_end(S_QuoteMark);
        } else if ((f & S_ListMark) && (f & S_Marker)) {
            if (!info.task) emit("\xE2\x80\xA2 ", S_ListMark, i);  // "• "
            i = run_end(S_ListMark);
        } else if (f & S_TaskBox) {
            // One placeholder byte; the layout gives it the box's width.
            emit(" ", S_TaskBox | (info.done ? S_TaskDone : 0), i);
            i = run_end(S_TaskBox);
        } else if (f & S_Marker) {
            i++;
        } else {
            out += s[i]; ost.push_back(f); map.push_back(i);
            i++;
        }
    }
}
