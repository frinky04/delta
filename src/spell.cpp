#include "spell.h"

#include <cctype>
#include <cstring>

#include "markdown.h"
#include "util.h"

namespace {

// Letters of any script, roughly: everything past ASCII that isn't
// punctuation, a symbol or an emoji.
bool is_letter(unsigned cp) {
    if (cp < 0x80) return (cp | 32) >= 'a' && (cp | 32) <= 'z';
    if (cp < 0xC0 || cp == 0xD7 || cp == 0xF7) return false;  // Latin-1 punctuation and signs
    if (cp >= 0x2000 && cp < 0x2C00) return false;             // punctuation, symbols, arrows, shapes
    if (cp >= 0x3000 && cp < 0x3040) return false;             // CJK punctuation
    if (cp >= 0xE000 && cp < 0xF900) return false;             // private use
    if (cp >= 0xFE00 && cp < 0xFE10) return false;             // variation selectors
    if (cp >= 0xFFF0 && cp < 0x10000) return false;            // specials
    if (cp >= 0x1F000) return false;                           // emoji
    return true;
}
bool is_digit(unsigned cp) { return cp >= '0' && cp <= '9'; }
bool is_apostrophe(unsigned cp) { return cp == '\'' || cp == 0x2019; }
bool is_ascii_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_ascii_upper(char c) { return c >= 'A' && c <= 'Z'; }

unsigned cp_at(const std::string& s, int i, int end, int& len) {
    return utf8::decode(s.data() + i, s.data() + end, &len);
}

// A run of non-space text that reads as code or an address rather than
// words: a url, email or @mention, a path, a #tag, a snake_case name, a
// file name or domain (a dot between letters), or code punctuation.
bool codeish(const std::string& s, int a, int b) {
    if (s[a] == '#' || s.substr(a, b - a).find("://") != std::string::npos) return true;
    for (int k = a; k < b; k++) {
        char c = s[k];
        if (c && std::strchr("@/\\_{}<>=$|~^", c)) return true;
        if ((c == '.' || c == ':') && k > a && k + 1 < b && md_word_char(s[k - 1]) && md_word_char(s[k + 1])) return true;
    }
    return false;
}

// Words that are names or shouting rather than prose: ALL CAPS, or a
// capital straight after a small letter (camelCase, iPhone).
bool name_like(const std::string& s, int a, int b) {
    int upper = 0, lower = 0;
    for (int k = a; k < b; k++) {
        if (is_ascii_upper(s[k])) {
            upper++;
            if (k > a && is_ascii_lower(s[k - 1])) return true;
        } else if (is_ascii_lower(s[k])) {
            lower++;
        }
    }
    return upper >= 2 && lower == 0;
}

}  // namespace

std::vector<bool> spell_skipped(const std::vector<std::string>& lines) {
    std::vector<bool> out(lines.size(), false);
    size_t i = 0;
    // Front matter: a --- first line, closed by --- or ... (unclosed, it's a rule).
    if (!lines.empty() && trim(lines[0]) == "---") {
        for (size_t k = 1; k < lines.size(); k++) {
            std::string t = trim(lines[k]);
            if (t != "---" && t != "...") continue;
            for (size_t j = 0; j <= k; j++) out[j] = true;
            i = k + 1;
            break;
        }
    }
    bool in_code = false;
    for (; i < lines.size(); i++) {
        bool fence = md_is_fence(lines[i]);
        if (in_code || fence) out[i] = true;
        if (fence) in_code = !in_code;
    }
    return out;
}

std::vector<WordSpan> spell_words(const std::string& s) {
    std::vector<WordSpan> out;
    LineInfo info;
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    md_analyze(s, false, info, st, &links);
    if (info.kind != LineKind::Para && info.kind != LineKind::Heading) return out;

    // What markdown says isn't prose: syntax, code, urls, tags.
    int n = (int)s.size();
    std::vector<char> skip(n, 0);
    const Style never = S_Marker | S_Code | S_CodeBlock | S_Url | S_TaskBox | S_ListMark | S_Tag;
    for (int i = 0; i < n; i++) skip[i] = (st[i] & never) != 0;
    // A [[link]] without an |alias shows its target: a note's name.
    for (auto& l : links)
        if (l.wiki && l.end - l.begin - 4 == (int)l.target.size())
            for (int i = l.begin; i < l.end; i++) skip[i] = 1;
    // Html tags.
    for (int i = 0; i + 1 < n; i++) {
        if (s[i] != '<' || !(std::isalpha((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) continue;
        size_t e = s.find('>', i);
        if (e == std::string::npos) break;
        for (int k = i; k <= (int)e; k++) skip[k] = 1;
        i = (int)e;
    }

    // Runs of non-space text; within those that read as prose, runs of
    // letters (and digits, to catch words that have them) joined by
    // apostrophes. Hyphens and dashes split words.
    for (int i = 0; i < n;) {
        if (skip[i] || s[i] == ' ' || s[i] == '\t') { i++; continue; }
        int a = i;
        while (i < n && !skip[i] && s[i] != ' ' && s[i] != '\t') i++;
        int b = i;
        if (codeish(s, a, b)) continue;
        for (int k = a; k < b;) {
            int len;
            unsigned cp = cp_at(s, k, b, len);
            if (!is_letter(cp) && !is_digit(cp)) { k += len; continue; }
            int w = k, chars = 0;
            bool digit = false;
            while (k < b) {
                cp = cp_at(s, k, b, len);
                if (is_letter(cp) || is_digit(cp)) {
                    digit |= is_digit(cp);
                    chars++;
                    k += len;
                    continue;
                }
                int len2;
                if (is_apostrophe(cp) && k + len < b && is_letter(cp_at(s, k + len, b, len2))) { k += len; continue; }
                break;
            }
            if (!digit && chars >= 2 && !name_like(s, w, k)) out.push_back({w, k});
        }
    }
    return out;
}

std::string spell_form(const std::string& line, WordSpan w) {
    std::string out = line.substr(w.begin, w.end - w.begin);
    for (size_t p; (p = out.find("\xE2\x80\x99")) != std::string::npos;) out.replace(p, 3, "'");
    return out;
}
