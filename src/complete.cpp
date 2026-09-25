#include "complete.h"

#include <algorithm>
#include <cctype>

#include "palette.h"
#include "util.h"

bool starts_word(const std::string& s, const std::vector<int>& hl) {
    if (hl.empty()) return true;
    int i = hl.front();
    return i <= 0 || i > (int)s.size() || !std::isalnum((unsigned char)s[i - 1]);
}

// The best 50, highest first; `before` orders equals.
template <class Less>
static std::vector<CompletionItem> best(std::vector<std::pair<int, CompletionItem>>& ranked, Less before) {
    std::stable_sort(ranked.begin(), ranked.end(), before);
    std::vector<CompletionItem> out;
    for (auto& r : ranked) {
        out.push_back(std::move(r.second));
        if (out.size() >= 50) break;
    }
    return out;
}

std::vector<CompletionItem> complete_tags(const TagIndex& ix, const fs::path& open, const std::vector<std::string>& lines,
                                          TextPos caret, const std::string& word) {
    TagTally all = ix.tally([&](const TagIndex::Entry& e) { return open.empty() || e.note.path != open; });
    if (!open.empty()) {
        TagSet mine;
        for (auto& t : find_tags(lines))
            if (t.line != caret.line || t.col1 != caret.col) mine.emplace(lower(t.name), t.name);
        count_tags(all, mine);
    }
    std::vector<std::pair<int, CompletionItem>> ranked;
    for (auto& [k, t] : all) {
        CompletionItem it;
        int score = 0;
        if (!fuzzy_match(word, t.name, score, &it.hl) || !starts_word(t.name, it.hl)) continue;
        for (int& h : it.hl) h++;  // past the #
        it.label = "#" + t.name;
        it.insert = t.name;
        it.meta = std::to_string(t.notes);
        ranked.push_back({score + std::min(t.notes, 50) * 2, std::move(it)});  // a used tag outranks a slightly shorter one
    }
    return best(ranked, [](auto& a, auto& b) { return a.first > b.first; });
}

std::vector<CompletionItem> complete_links(const NoteIndex& ix, const std::string& space, const std::string& own,
                                           const std::string& word) {
    std::vector<std::pair<int, CompletionItem>> ranked;
    for (auto& [key, sp] : ix.spaces) {
        bool here = iequals(sp.name, space);
        for (auto& [nk, name] : sp.notes) {
            if (here && iequals(name, own)) continue;  // not to itself
            CompletionItem it;
            it.label = here ? name : sp.name + "/" + name;
            int score = 0;
            if (!fuzzy_match(word, it.label, score, &it.hl) || !starts_word(it.label, it.hl)) continue;
            it.insert = it.label;
            it.dim = here ? 0 : (int)sp.name.size() + 1;
            ranked.push_back({score + (here ? 1 : 0), std::move(it)});
        }
    }
    // Among equals, this space's first, then by name.
    return best(ranked, [](auto& a, auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return natural_less(a.second.label, b.second.label);
    });
}
