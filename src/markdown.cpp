#include "markdown.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

static bool is_ws(char c) { return c == ' ' || c == '\t'; }

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

bool md_word_char(unsigned char c) { return std::isalnum(c) || c >= 0x80; }

bool md_tag_char(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '/' || c >= 0x80; }

int md_tag_end(const std::string& s, int at) {
    const int n = (int)s.size();
    if (at < 1 || at > n || s[at - 1] != '#') return at;
    // Not a word's #, a run of #s or an html entity (&#39;).
    if (at >= 2) {
        unsigned char b = (unsigned char)s[at - 2];
        if (md_word_char(b) || b == '#' || b == '&') return at;
    }
    int e = at;
    while (e < n && md_tag_char((unsigned char)s[e])) e++;
    while (e > at && s[e - 1] == '/') e--;  // "#a/" ends a sentence, not a path
    for (int k = at; k < e; k++)
        if (std::isalpha((unsigned char)s[k]) || (unsigned char)s[k] >= 0x80) return e;
    return at;
}

int md_link_word_start(const std::string& s, int col, const std::vector<Style>& st) {
    const int n = (int)s.size();
    if (col < 2 || col > n) return -1;
    size_t open = s.rfind("[[", (size_t)col - 2);
    if (open == std::string::npos) return -1;
    int from = (int)open + 2;
    // Still in the target: not past its end, an |alias or a #heading.
    if (s.substr(from, col - from).find_first_of("[]|#") != std::string::npos) return -1;
    if (open < st.size() && (st[open] & (S_Code | S_CodeBlock))) return -1;
    // At the end of what's typed so far.
    if (col < n && s.compare(col, 2, "]]") != 0 && !is_ws(s[col])) return -1;
    return from;
}

// Where the alt text of the image at s[a] ends: the matching "]" (brackets
// inside it must balance), or past the end if it's never closed.
static size_t alt_close(const std::string& s, size_t a) {
    size_t i = a + 2;
    int depth = 0;
    for (; i < s.size(); i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '[') depth++;
        else if (s[i] == ']' && depth-- == 0) break;
    }
    return i;
}

// One ![alt](src) starting at s[a]; sets `end` past its ")".
bool md_image_at(const std::string& s, size_t a, ImageRef& ref) {
    size_t n = s.size();
    if (a + 5 > n || s.compare(a, 2, "![") != 0) return false;
    size_t i = alt_close(s, a);
    if (i + 1 >= n || s[i + 1] != '(') return false;
    size_t alt_end = i;
    // The target runs to the ")" that closes it: past <...> if bracketed,
    // past a "title" if there is one.
    size_t t0 = i + 2, j = t0;
    std::string path;
    size_t path_at = t0;
    if (j < n && s[j] == '<') {
        size_t close = s.find('>', j);
        if (close == std::string::npos) return false;
        path_at = j + 1;
        path = s.substr(j + 1, close - j - 1);
        j = close + 1;
    } else {
        while (j < n && s[j] != ')' && !is_ws(s[j])) j++;
        path = s.substr(t0, j - t0);
    }
    while (j < n && is_ws(s[j])) j++;
    if (j < n && s[j] == '"') {
        size_t q = s.find('"', j + 1);
        if (q == std::string::npos) return false;
        j = q + 1;
        while (j < n && is_ws(s[j])) j++;
    }
    if (j >= n || s[j] != ')' || path.empty() || path.find(')') != std::string::npos) return false;
    ref.begin = (int)a;
    ref.end = (int)j + 1;
    ref.src = path;
    ref.src_at = (int)path_at;
    ref.alt = s.substr(a + 2, alt_end - (a + 2));
    ref.w = ref.h = 0;
    // "alt|300" or "alt|300x200": Obsidian's size.
    size_t bar = ref.alt.rfind('|');
    if (bar != std::string::npos) {
        std::string size = ref.alt.substr(bar + 1);
        size_t x = size.find('x');
        auto digits = [](const std::string& d) { return !d.empty() && d.size() <= 5 && std::all_of(d.begin(), d.end(), [](char c) { return c >= '0' && c <= '9'; }); };
        std::string ws = size.substr(0, x), hs = x == std::string::npos ? std::string() : size.substr(x + 1);
        if (digits(ws) && (x == std::string::npos || digits(hs))) {
            ref.w = std::atoi(ws.c_str());
            ref.h = hs.empty() ? 0 : std::atoi(hs.c_str());
            ref.alt = ref.alt.substr(0, bar);
        }
    }
    return true;
}

bool md_image_row(const std::string& s, std::vector<ImageRef>* out) {
    if (out) out->clear();
    size_t i = 0, n = s.size();
    int count = 0;
    while (true) {
        while (i < n && is_ws(s[i])) i++;
        if (i >= n) break;
        ImageRef ref;
        if (!md_image_at(s, i, ref)) return false;
        // Images need space between them, or they'd be one word.
        if (count > 0 && !is_ws(s[i - 1])) return false;
        if (out) out->push_back(ref);
        count++;
        i = ref.end;
    }
    return count > 0;
}

bool md_image_line(const std::string& s, std::string* alt, std::string* src) {
    if (!alt && !src) return md_image_row(s);  // the usual question: no list to build
    std::vector<ImageRef> refs;
    if (!md_image_row(s, &refs)) return false;
    if (alt) *alt = refs[0].alt;
    if (src) *src = refs[0].src;
    return true;
}

std::string md_image_text(const std::string& alt, const std::string& src, int w, int h) {
    std::string a = alt;
    if (w > 0) a += "|" + std::to_string(w) + (h > 0 ? "x" + std::to_string(h) : std::string());
    bool spaced = src.find_first_of(" \t") != std::string::npos;
    return "![" + a + "](" + (spaced ? "<" + src + ">" : src) + ")";
}

std::string md_image_resized(const std::string& token, int w) {
    ImageRef ref;
    if (!md_image_at(token, 0, ref)) return token;
    // Keep everything after the alt text (the target, any title) as it was.
    std::string alt = ref.alt;
    if (w > 0) alt += "|" + std::to_string(w);
    return "![" + alt + token.substr(alt_close(token, 0));
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

static void mark(std::vector<Style>& st, size_t a, size_t b, Style f) {
    for (size_t i = a; i < b && i < st.size(); i++) st[i] |= f;
}

static void inline_parse(const std::string& s, size_t from, std::vector<Style>& st, std::vector<LinkSpan>* links) {
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
        if (!len || (i > 0 && md_word_char((unsigned char)s[i - 1]))) continue;
        size_t e = i + len;
        while (e < n && !is_ws(s[e]) && !used[e] && s[e] != '>' && s[e] != ')') e++;
        while (e > i + len && std::strchr(".,;:!?", s[e - 1])) e--;
        if (e == i + len) continue;
        mark(st, i, e, S_Link);
        if (links) links->push_back({(int)i, (int)e, s.substr(i, e - i), false});
        use(i, e);
        i = e;
    }

    // #tags: after links, so a url's #fragment isn't one.
    for (size_t i = from; i + 1 < n; i++) {
        if (used[i] || s[i] != '#') continue;
        size_t e = (size_t)md_tag_end(s, (int)i + 1);
        if (e == i + 1 || any_used(i, e)) continue;
        mark(st, i, e, S_Tag);
        if (links) links->push_back({(int)i, (int)e, s.substr(i + 1, e - i - 1), false, true});
        use(i, e);
        i = e - 1;
    }

    // Emphasis. Longer delimiters first so ** isn't eaten as two *s.
    struct Delim { const char* d; size_t len; Style flag; bool intraword_ok; };
    static const Delim delims[] = {
        {"**", 2, S_Bold, true}, {"__", 2, S_Bold, false}, {"~~", 2, S_Strike, true},
        {"==", 2, S_Highlight, true}, {"*", 1, S_Italic, true}, {"_", 1, S_Italic, false},
    };
    for (const Delim& d : delims) {
        auto at = [&](size_t i) { return i + d.len <= n && s.compare(i, d.len, d.d) == 0 && !any_used(i, i + d.len); };
        for (size_t i = from; i + d.len <= n;) {
            size_t a = i + d.len;
            bool open = at(i) && a < n && !is_ws(s[a]) && s[a] != d.d[0] &&
                        (d.intraword_ok || i == 0 || !md_word_char((unsigned char)s[i - 1]));
            if (!open) { i++; continue; }
            size_t close = std::string::npos;
            for (size_t j = a + 1; j + d.len <= n; j++) {
                if (!at(j) || is_ws(s[j - 1])) continue;
                if (!d.intraword_ok && j + d.len < n && md_word_char((unsigned char)s[j + d.len])) continue;
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

void md_analyze(const std::string& s, bool in_code, LineInfo& info, std::vector<Style>& st, std::vector<LinkSpan>* links) {
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
    if (md_image_line(s)) {
        info.kind = LineKind::Image;
        mark(st, 0, n, S_Marker);
        return;
    }
    if (is_rule(s)) {
        info.kind = LineKind::Rule;
        mark(st, 0, n, S_Marker);
        return;
    }

    if (i <= 3 && s[i] == '#') {
        size_t j = i;
        while (j < n && s[j] == '#') j++;
        // Only once a space follows: a # just typed may be a #tag's.
        if (j - i <= 6 && j < n && s[j] == ' ') {
            info.kind = LineKind::Heading;
            info.level = (int)(j - i);
            size_t content = j + 1;
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

std::vector<bool> md_code_lines(const std::vector<std::string>& lines) {
    std::vector<bool> out(lines.size(), false);
    bool in_code = false;
    for (size_t i = 0; i < lines.size(); i++) {
        bool fence = md_is_fence(lines[i]);
        out[i] = in_code || fence;
        if (fence) in_code = !in_code;
    }
    return out;
}

std::vector<LineLink> md_links(const std::vector<std::string>& lines) {
    std::vector<LineLink> out;
    std::vector<bool> code = md_code_lines(lines);
    LineInfo info;
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& s = lines[i];
        // Most lines have nothing to find; skip them without analyzing.
        if (code[i] || (s.find_first_of("#[") == std::string::npos && s.find("http") == std::string::npos)) continue;
        links.clear();
        md_analyze(s, false, info, st, &links);
        for (auto& l : links) out.push_back({i, std::move(l)});
    }
    return out;
}

void md_reader(const std::string& s, const LineInfo& info, const std::vector<Style>& st,
               std::string& out, std::vector<Style>& ost, std::vector<int>& map) {
    out.clear(); ost.clear(); map.clear();
    auto emit = [&](const char* text, Style style, int src) {
        for (const char* c = text; *c; c++) { out += *c; ost.push_back(style); map.push_back(src); }
    };
    if (info.kind == LineKind::Fence || info.kind == LineKind::Rule || info.kind == LineKind::Image) return;
    if (info.kind == LineKind::CodeBlock) {
        out = s; ost = st;
        for (int i = 0; i < (int)s.size(); i++) map.push_back(i);
        return;
    }
    const int n = (int)s.size();
    for (int i = 0; i < n;) {
        Style f = st[i];
        auto run_end = [&](Style flag) { int j = i; while (j < n && (st[j] & flag)) j++; return j; };
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
