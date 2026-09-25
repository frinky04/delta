#include "tags.h"

#include <algorithm>
#include <set>

#include "markdown.h"
#include "util.h"

std::vector<TagUse> find_tags(const std::vector<std::string>& lines) {
    std::vector<TagUse> out;
    for (auto& [i, l] : md_links(lines))
        if (l.tag) out.push_back({i, l.begin, l.end, std::move(l.target)});
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
    if (md_tag_end("#" + name, 1) == 1) return "a tag needs a letter; #" + name + " would be a number";
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
    if (k - 1 < (int)st.size() && (st[k - 1] & (S_Code | S_CodeBlock | S_Url | S_Link | S_WikiLink))) return -1;
    if (k < col) {
        if (md_tag_end(line, k) == k) return -1;  // "#12", a word's #: not a tag
    } else if (k >= 2) {  // just the #: only what's before it can say
        unsigned char b = (unsigned char)line[k - 2];
        if (md_word_char(b) || b == '#' || b == '&') return -1;
    }
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

static int64_t stamp_from(fs::file_time_type t, std::uintmax_t size) {
    return (int64_t)t.time_since_epoch().count() * 31 + (int64_t)size;
}

static int64_t stamp_of(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto size = fs::file_size(p, ec);
    return stamp_from(t, ec ? 0 : size);
}

// From the folder listing when the ref came from one, else from the disk.
static int64_t stamp_of(const NoteRef& n) {
    return n.listed ? stamp_from(n.write_time, n.size) : stamp_of(n.path);
}

void TagIndex::refresh(const std::vector<NoteRef>& notes) {
    std::set<std::string> seen;
    for (auto& n : notes) {
        std::string key = path_u8(n.path);
        seen.insert(key);
        int64_t stamp = stamp_of(n);
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

void count_tags(TagTally& into, const TagSet& tags) {
    for (auto& [k, v] : tags) {
        TagCount& t = into[k];
        if (t.name.empty()) t.name = v;
        t.notes++;
    }
}

TagTally TagIndex::tally(const std::function<bool(const Entry&)>& include) const {
    TagTally out;
    for (auto& [path, e] : notes_)
        if (!include || include(e)) count_tags(out, e.tags);
    return out;
}

int TagIndex::notes_with(const std::string& tag) const {
    std::string key = lower(tag);
    int n = 0;
    for (auto& [path, e] : notes_) n += e.tags.count(key) ? 1 : 0;
    return n;
}
