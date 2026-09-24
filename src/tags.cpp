#include "tags.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "markdown.h"
#include "util.h"

static bool is_word(unsigned char c) { return std::isalnum(c) || c >= 0x80; }
static bool has_letter(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return std::isalpha((unsigned char)c) || (unsigned char)c >= 0x80; });
}

std::vector<TagUse> find_tags(const std::vector<std::string>& lines) {
    std::vector<TagUse> out;
    bool in_code = false;
    LineInfo info;
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& s = lines[i];
        bool fence = md_is_fence(s);
        if (!in_code && !fence && s.find('#') != std::string::npos) {
            links.clear();
            md_analyze(s, false, info, st, &links);
            for (auto& l : links)
                if (l.tag) out.push_back({i, l.begin, l.end, l.target});
        }
        if (fence) in_code = !in_code;
    }
    return out;
}

TagSet tags_of(const std::vector<std::string>& lines) {
    TagSet out;
    for (auto& t : find_tags(lines)) out.emplace(lower(t.name), t.name);
    return out;
}

std::string check_tag(const std::string& name) {
    if (name.empty()) return "a tag needs a name";
    for (char c : name)
        if (!md_tag_char((unsigned char)c)) return "a tag is letters, digits, _ - and /; no spaces";
    if (!has_letter(name)) return "a tag needs a letter; #" + name + " would be a number";
    if (name.back() == '/') return "a tag can't end with /";
    return {};
}

int tag_word_start(const std::string& line, int col, const std::vector<Style>& st) {
    const int n = (int)line.size();
    if (col < 0 || col > n) return -1;
    if (col < n && md_tag_char((unsigned char)line[col])) return -1;  // in the middle of a word
    int k = col;
    while (k > 0 && md_tag_char((unsigned char)line[k - 1])) k--;
    if (k < 1 || line[k - 1] != '#') return -1;
    if (k >= 2) {
        unsigned char b = (unsigned char)line[k - 2];
        if (is_word(b) || b == '#' || b == '&') return -1;
    }
    if (k - 1 < (int)st.size() && (st[k - 1] & (S_Code | S_CodeBlock | S_Url | S_Link | S_WikiLink))) return -1;
    if (k < col && !has_letter(line.substr(k, col - k))) return -1;  // "#12": a number, not a tag
    return k;
}

int rename_tag(std::vector<std::string>& lines, const std::string& from, const std::string& to, TextPos* caret) {
    std::vector<TagUse> uses = find_tags(lines);
    int count = 0;
    // From the last, so the ones before keep their places.
    for (auto it = uses.rbegin(); it != uses.rend(); ++it) {
        if (!iequals(it->name, from)) continue;
        int a = it->col0 + 1, b = it->col1;
        lines[it->line].replace(a, b - a, to);
        count++;
        if (caret && caret->line == it->line) {
            if (caret->col >= b) caret->col += (int)to.size() - (b - a);
            else if (caret->col > a) caret->col = a + std::min(caret->col - a, (int)to.size());
        }
    }
    return count;
}

TagQuery parse_tag_query(const std::string& text) {
    TagQuery q;
    std::vector<std::string> toks;
    std::string cur;
    for (char c : text) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) toks.push_back(std::move(cur)), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) toks.push_back(std::move(cur));
    bool open = text.empty() || (text.back() != ' ' && text.back() != '\t');
    if (toks.empty()) q.typing = open;
    for (size_t i = 0; i < toks.size(); i++) {
        std::string t = toks[i];
        bool skip = t[0] == '-';
        if (skip) t.erase(0, 1);
        while (!t.empty() && t[0] == '#') t.erase(0, 1);
        if (i + 1 == toks.size() && open) {
            q.typing = true;
            q.typing_skip = skip;
            q.word = t;
        } else if (!t.empty()) {
            (skip ? q.skip : q.want).push_back(lower(t));
        }
    }
    return q;
}

bool tag_query_match(const TagQuery& q, const TagSet& tags) {
    for (auto& w : q.want)
        if (!tags.count(w)) return false;
    for (auto& s : q.skip)
        if (tags.count(s)) return false;
    return true;
}

std::string tag_query_fill(const std::string& text, const std::string& name) {
    size_t cut = text.size();
    while (cut > 0 && text[cut - 1] != ' ' && text[cut - 1] != '\t') cut--;
    std::string head = text.substr(0, cut);
    bool skip = cut < text.size() && text[cut] == '-';
    bool first = trim(head).empty();
    return head + (skip ? "-" : "") + (first ? "" : "#") + name + " ";
}

static int64_t stamp_of(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto size = fs::file_size(p, ec);
    return (int64_t)t.time_since_epoch().count() * 31 + (ec ? 0 : (int64_t)size);
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines(1);
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') lines.emplace_back();
        else lines.back() += c;
    }
    return lines;
}

void TagIndex::refresh(const std::vector<NoteRef>& notes) {
    std::set<std::string> seen;
    for (auto& n : notes) {
        std::string key = path_u8(n.path);
        seen.insert(key);
        int64_t stamp = stamp_of(n.path);
        auto it = notes_.find(key);
        if (it != notes_.end() && it->second.stamp == stamp) {
            it->second.note = n;
            continue;
        }
        std::string text;
        read_file(n.path, text);
        notes_[key] = {n, tags_of(split_lines(text)), stamp};
    }
    for (auto it = notes_.begin(); it != notes_.end();) {
        if (seen.count(it->first)) ++it;
        else it = notes_.erase(it);
    }
}

void TagIndex::set(const NoteRef& n, const std::vector<std::string>& lines) {
    notes_[path_u8(n.path)] = {n, tags_of(lines), stamp_of(n.path)};
}
