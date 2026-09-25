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

// Each [[link]] in these lines outside code, with its line and its target
// as space and name. Links with no name are skipped.
template <class F>
static void for_each_link(const std::vector<std::string>& lines, F fn) {
    bool in_code = false;
    LineInfo info;
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    for (int i = 0; i < (int)lines.size(); i++) {
        const std::string& s = lines[i];
        bool fence = md_is_fence(s);
        if (!in_code && !fence && s.find("[[") != std::string::npos) {
            links.clear();
            md_analyze(s, false, info, st, &links);
            for (auto& l : links) {
                std::string space, name;
                if (l.wiki && Store::split_target(l.target, space, name)) fn(i, l, space, name);
            }
        }
        if (fence) in_code = !in_code;
    }
}

std::vector<Diagnostic> link_problems(const std::vector<std::string>& lines, const NoteIndex& ix) {
    std::vector<Diagnostic> out;
    for_each_link(lines, [&](int i, const LinkSpan& l, const std::string& space, const std::string& name) {
        if (ix.has(space, name)) return;
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
    });
    return out;
}

std::vector<LinkHit> links_to(const std::vector<std::string>& lines, const std::string& from,
                              const std::string& space, const std::string& name, const NoteIndex& ix) {
    std::vector<LinkHit> out;
    for_each_link(lines, [&](int i, const LinkSpan& l, const std::string& sp, const std::string& n) {
        std::string to_sp, to_n;
        if (!ix.resolve(from, sp, n, to_sp, to_n) || !iequals(to_sp, space) || !iequals(to_n, name)) return;
        out.push_back({i, l.begin + 2, l.begin + 2 + (int)l.target.size()});
    });
    return out;
}
