#include "ini_syntax.h"

#include <cctype>

#include "util.h"

static void mark(std::vector<uint16_t>& st, size_t a, size_t b, uint16_t f) {
    for (size_t i = a; i < b && i < st.size(); i++) st[i] |= f;
}

static bool is_hex_colour(const std::string& v) {
    if (v.size() != 7 || v[0] != '#') return false;
    for (size_t i = 1; i < 7; i++)
        if (!std::isxdigit((unsigned char)v[i])) return false;
    return true;
}

// Start of a " ; comment" tail, or npos.
static size_t inline_comment(const std::string& s, size_t from) {
    for (size_t i = from; i < s.size(); i++)
        if (s[i] == ';' && i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\t')) return i;
    return std::string::npos;
}

void ini_analyze(const std::string& s, LineInfo& info, std::vector<uint16_t>& st) {
    const size_t n = s.size();
    st.assign(n, S_CodeBlock);
    info = LineInfo{};
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i == n) { info.kind = LineKind::Blank; return; }
    info.kind = LineKind::Para;

    if (s[i] == ';' || s[i] == '#') {
        mark(st, i, n, I_Comment);
        return;
    }
    if (s[i] == '[') {
        size_t close = s.find(']', i);
        mark(st, i, i + 1, I_Bracket);
        if (close == std::string::npos) { mark(st, i + 1, n, I_Section); return; }
        mark(st, i + 1, close, I_Section);
        mark(st, close, close + 1, I_Bracket);
        size_t c = inline_comment(s, close);
        if (c != std::string::npos) mark(st, c, n, I_Comment);
        return;
    }
    size_t eq = s.find('=', i);
    if (eq == std::string::npos) { mark(st, i, n, I_Key); return; }
    mark(st, i, eq, I_Key);
    mark(st, eq, eq + 1, I_Eq);
    size_t c = inline_comment(s, eq + 1);
    size_t vend = c == std::string::npos ? n : c;
    if (c != std::string::npos) mark(st, c, n, I_Comment);
    size_t va = eq + 1;
    while (va < vend && (s[va] == ' ' || s[va] == '\t')) va++;
    size_t vb = vend;
    while (vb > va && (s[vb - 1] == ' ' || s[vb - 1] == '\t')) vb--;
    mark(st, va, vb, I_Value);
    if (is_hex_colour(s.substr(va, vb - va))) mark(st, va, vb, I_Color);
}

bool ini_section(const std::string& s, std::string& name) {
    std::string t = trim(s);
    if (t.size() < 2 || t.front() != '[') return false;
    size_t close = t.find(']');
    if (close == std::string::npos) return false;
    name = trim(t.substr(1, close - 1));
    return true;
}
