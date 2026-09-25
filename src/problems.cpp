#include "problems.h"

#include "config.h"
#include "markdown.h"
#include "util.h"

// The name suggest() would pick for a typo, from names offered one at a time
// as a lower-cased key and the name as written, in any order: ties go to the
// smaller key, and a key offered again keeps its first name, as if they were
// all gathered in one map first.
namespace {
struct Nearest {
    std::string typo;
    int limit, best_d;
    const std::string* best_key = nullptr;
    const std::string* best = nullptr;

    explicit Nearest(std::string t) : typo(std::move(t)), limit(suggest_limit(typo)), best_d(limit + 1) {}
    void offer(const std::string& key, const std::string& name) {
        int d = edit_distance(typo, key, best_d);
        if (d > limit) return;
        if (d < best_d || (d == best_d && key < *best_key)) {
            best_d = d;
            best_key = &key;
            best = &name;
        }
    }
    std::string result() const { return best ? *best : std::string(); }
};
}  // namespace

// The closest space name to a mistyped one, or "".
static std::string closest_space(const std::string& typo, const NoteIndex& ix) {
    Nearest n(lower(typo));
    for (auto& [k, sp] : ix.spaces) n.offer(k, sp.name);
    return n.result();
}

// The closest note name to a mistyped one, in `space` ("" for every space),
// or "".
static std::string closest_note(const std::string& space, const std::string& typo, const NoteIndex& ix) {
    Nearest n(lower(typo));
    std::string ls = lower(space);
    for (auto& [k, sp] : ix.spaces)
        if (space.empty() || k == ls)
            for (auto& [nk, name] : sp.notes) n.offer(nk, name);
    return n.result();
}

// Either of those, remembered in the index: a link being typed asks again
// every keystroke. `kind` is 's' for a space, 'n' for a note.
static std::string closest(char kind, const std::string& space, const std::string& typo, const NoteIndex& ix) {
    auto& cache = ix.near_cache.near;
    std::string key = std::string(1, kind) + '\0' + lower(space) + '\0' + lower(typo);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    if (cache.size() > 4096) cache.clear();  // a long session's typing
    std::string near = kind == 's' ? closest_space(space, ix) : closest_note(space, typo, ix);
    cache.emplace(std::move(key), near);
    return near;
}

// Each [[link]] in these lines outside code, with its line and its target
// as space and name. Links with no name are skipped.
template <class F>
static void for_each_link(const std::vector<std::string>& lines, F fn) {
    for (auto& [i, l] : md_links(lines)) {
        std::string space, name;
        if (l.wiki && Store::split_target(l.target, space, name)) fn(i, l, space, name);
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
            near = closest('s', space, "", ix);
            d.msg = "no space with this name yet";
        } else {
            near = closest('n', space, name, ix);
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
