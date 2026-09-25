#include "commands.h"

#include <cctype>
#include <map>

#include "palette.h"
#include "util.h"

const char* command_keywords(const std::string& id) {
    static const std::map<std::string, const char*> words = {
        {"palette.notes", "open goto"},
        {"palette.all", "open everywhere"},
        {"palette.sort", "order alphabetical"},
        {"palette.spaces", "folder workspace"},
        {"palette.headings", "outline toc section goto"},
        {"palette.line", "goto"},
        {"palette.search", "grep text"},
        {"palette.backlinks", "incoming references"},
        {"palette.tags", "hashtag label"},
        {"palette.widen", "all everywhere scope narrow"},
        {"tag.rename", "hashtag label merge"},
        {"nav.heading_next", "section"},
        {"nav.heading_prev", "section"},
        {"note.delete", "remove"},
        {"note.restore", "trash undelete recover"},
        {"note.save", "write"},
        {"note.reload", "revert disk discard conflict"},
        {"note.keep", "import save copy visiting"},
        {"view.reader", "preview read"},
        {"view.zoom_in", "bigger larger"},
        {"view.zoom_out", "smaller"},
        {"view.fullscreen", "maximise maximize"},
        {"config.open", "settings preferences options"},
        {"config.add_setting", "preferences option"},
        {"font.choose", "typeface"},
        {"font.choose_mono", "typeface monospace code"},
        {"nav.next_misspelling", "spell typo"},
        {"nav.prev_misspelling", "spell typo"},
        {"edit.quick_fix", "spell typo"},
        {"edit.fix_spelling", "typo"},
        {"view.spelling", "typo"},
        {"app.open_folder", "explorer finder reveal"},
        {"app.manual", "help docs readme"},
        {"app.quit", "exit close"},
        {"image.view", "picture photo"},
        {"edit.toggle_task", "todo done"},
        {"edit.code", "monospace backtick"},
    };
    auto it = words.find(id);
    return it == words.end() ? "" : it->second;
}

// The words of `s` (split at anything that isn't a letter or digit), lower-cased.
static std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    std::string w;
    for (char c : s) {
        if (std::isalnum((unsigned char)c)) w += (char)std::tolower((unsigned char)c);
        else if (!w.empty()) out.push_back(std::move(w)), w.clear();
    }
    if (!w.empty()) out.push_back(std::move(w));
    return out;
}

bool match_command(const std::string& query, const std::string& title, const std::string& id, const std::string& keywords,
                   int& score, std::vector<int>* hl, std::string* via) {
    score = 0;
    std::vector<std::string> qs = words_of(query);
    if (qs.empty()) return true;
    const std::string t = lower(title);
    std::vector<std::string> id_words = words_of(id), key_words = words_of(keywords);
    auto starts = [](const std::string& word, const std::string& w) { return word.compare(0, w.size(), w) == 0; };

    std::vector<int> pos;
    std::string found_via;
    bool all = true;
    for (auto& w : qs) {
        int best = 0;
        std::vector<int> best_pos;
        std::string best_via;
        // A title word it starts, earlier words a little better.
        for (size_t i = 0; i < t.size(); i++) {
            bool word_start = i == 0 || !std::isalnum((unsigned char)t[i - 1]);
            if (!word_start || t.compare(i, w.size(), w) != 0) continue;
            best = 300 - (int)std::min<size_t>(i, 40);
            for (size_t k = 0; k < w.size(); k++) best_pos.push_back((int)(i + k));
            break;
        }
        if (!best) {
            if (size_t at = t.find(w); at != std::string::npos) {
                best = 120;
                for (size_t k = 0; k < w.size(); k++) best_pos.push_back((int)(at + k));
            }
        }
        if (!best)
            for (auto& iw : id_words)
                if (starts(iw, w)) { best = 200, best_via = iw; break; }
        if (!best)
            for (auto& kw : key_words)
                if (starts(kw, w)) { best = 200, best_via = kw; break; }
        if (!best) { all = false; break; }
        score += best;
        pos.insert(pos.end(), best_pos.begin(), best_pos.end());
        if (!best_via.empty() && found_via.empty()) found_via = best_via;
    }

    if (!all) {
        // Letters in order across the title, for an abbreviation.
        if (qs.size() > 1 || !fuzzy_match(query, title, score, hl)) return false;
        score -= 2000;
        return true;
    }
    // The query as typed, in one piece, beats the same words apart.
    std::string phrase = lower(trim(query));
    if (qs.size() > 1 && t.find(phrase) != std::string::npos) score += 150;
    score -= (int)t.size();  // among equals, the shorter title
    if (hl) *hl = std::move(pos);
    if (via) *via = found_via;
    return true;
}
