#include "problems.h"

#include "config.h"
#include "markdown.h"
#include "util.h"

// The closest of `names` (a lower-cased key to the name as written), if it's
// close enough to be a typo.
static std::string closest(const std::string& typo, const std::map<std::string, std::string>& names) {
    std::vector<std::string> keys;
    for (auto& [k, v] : names) keys.push_back(k);
    std::string near = suggest(lower(typo), keys);
    return near.empty() ? near : names.at(near);
}

std::vector<Diagnostic> link_problems(const std::vector<std::string>& lines, const NoteIndex& ix) {
    std::vector<Diagnostic> out;
    bool in_code = false;
    LineInfo info;
    std::vector<uint16_t> st;
    std::vector<LinkSpan> links;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& s = lines[i];
        bool fence = md_is_fence(s);
        if (!in_code && !fence && s.find("[[") != std::string::npos) {
            links.clear();
            md_analyze(s, false, info, st, &links);
            for (auto& l : links) {
                std::string space, name;
                if (!l.wiki || !Store::split_target(l.target, space, name) || ix.has(space, name)) continue;
                Diagnostic d;
                d.line = i;
                d.col0 = l.begin + 2;
                d.col1 = d.col0 + (int)l.target.size();
                std::string near;
                if (!space.empty() && !ix.spaces.count(lower(space))) {
                    std::map<std::string, std::string> spaces;
                    for (auto& [k, sp] : ix.spaces) spaces[k] = sp.name;
                    near = closest(space, spaces);
                    d.msg = "no space with this name yet";
                } else {
                    std::map<std::string, std::string> notes;
                    for (auto& [k, sp] : ix.spaces)
                        if (space.empty() || k == lower(space)) notes.insert(sp.notes.begin(), sp.notes.end());
                    near = closest(name, notes);
                    d.msg = "no note with this name yet";
                }
                if (!near.empty()) d.msg += "; did you mean " + near + "?";
                out.push_back(d);
            }
        }
        if (fence) in_code = !in_code;
    }
    return out;
}

std::vector<LinkHit> links_to(const std::vector<std::string>& lines, const std::string& from,
                              const std::string& space, const std::string& name, const NoteIndex& ix) {
    std::vector<LinkHit> out;
    bool in_code = false;
    LineInfo info;
    std::vector<uint16_t> st;
    std::vector<LinkSpan> links;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& s = lines[i];
        bool fence = md_is_fence(s);
        if (!in_code && !fence && s.find("[[") != std::string::npos) {
            links.clear();
            md_analyze(s, false, info, st, &links);
            for (auto& l : links) {
                std::string sp, n, to_sp, to_n;
                if (!l.wiki || !Store::split_target(l.target, sp, n) || !ix.resolve(from, sp, n, to_sp, to_n)) continue;
                if (!iequals(to_sp, space) || !iequals(to_n, name)) continue;
                out.push_back({i, l.begin + 2, l.begin + 2 + (int)l.target.size()});
            }
        }
        if (fence) in_code = !in_code;
    }
    return out;
}
