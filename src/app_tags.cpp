// Tags: the index kept fresh, completing #tags and [[links]] as you write,
// the palette's # mode, and renaming a tag in every note. The index is
// tags.h's; ranking completions is complete.h's.
#include <algorithm>

#include "app.h"
#include "complete.h"
#include "textfile.h"
#include "theme.h"
#include "util.h"

void App::refresh_tags(bool now) {
    double t = ImGui::GetTime();
    if (!now && t - tags_time_ < 2.0) return;
    tags_time_ = t;
    tags_.refresh(store_.all_notes());
}

void App::tags_now() {
    refresh_tags(true);
    if (is_note()) tags_.set(*note_, editor_.lines());
}

std::vector<CompletionItem> App::tag_completions(const std::string& word) {
    refresh_tags(false);
    static const std::vector<std::string> none;
    bool n = is_note();
    return complete_tags(tags_, n ? note_->path : fs::path(), n ? editor_.lines() : none, editor_.cursor(), word);
}

std::vector<CompletionItem> App::link_completions(const std::string& word) {
    return complete_links(note_index_, space_, is_note() ? note_->name : std::string(), word);
}

// The palette's # mode. While a tag is being typed: the tags (on notes with
// the ones before it), each taken into the query. Once they're all
// finished: the notes that have them.
void App::build_tags(const std::string& query, std::vector<PaletteItem>& out, int& total) {
    if (tags_gen_ != palette_.generation()) {
        tags_gen_ = palette_.generation();
        tags_now();
    }
    TagQuery tq = parse_tag_query(query);
    bool wide = palette_.wide();
    std::vector<const TagIndex::Entry*> scope;
    for (auto& [path, e] : tags_.notes())
        if (wide || iequals(e.note.space, space_)) scope.push_back(&e);
    std::string elsewhere = wide ? "" : "; tab looks in every space";

    if (tq.typing) {
        TagTally tags = tags_.tally([&](const TagIndex::Entry& e) {
            return (wide || iequals(e.note.space, space_)) && tag_query_match(tq, e.tags);
        });
        for (auto& k : tq.want) tags.erase(k);  // chosen already
        for (auto& k : tq.skip) tags.erase(k);
        total = (int)tags.size();
        std::vector<PaletteItem> rows;
        for (auto& [k, t] : tags) {
            PaletteItem it;
            if (!fuzzy_match(tq.word, t.name, it.score, &it.hl)) continue;
            for (int& h : it.hl) h++;
            it.label = "#" + t.name;
            it.meta = count_of(t.notes, "note");
            it.fill = "#" + tag_query_fill(query, t.name);
            it.action = [] {};
            rows.push_back(std::move(it));
        }
        if (!tq.word.empty())
            std::stable_sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.score > b.score; });
        for (auto& r : rows) out.push_back(std::move(r));
        if (!tags.empty()) palette_.set_empty("no tag matches " + quote(tq.word));
        else if (tq.want.empty() && tq.skip.empty())
            palette_.set_empty(wide ? "no tags yet; write #tag in a note to add one" : "no tags in " + space_ + elsewhere);
        else palette_.set_empty("no other tags on these notes");
        return;
    }

    std::vector<const TagIndex::Entry*> hits;
    for (auto* e : scope)
        if (tag_query_match(tq, e->tags)) hits.push_back(e);
    auto label = [&](const NoteRef& n) { return wide ? n.space + "/" + n.name : n.name; };
    std::sort(hits.begin(), hits.end(), [&](auto* a, auto* b) { return natural_less(label(a->note), label(b->note)); });
    total = (int)scope.size();
    for (auto* e : hits) {
        const NoteRef& n = e->note;
        PaletteItem it;
        it.label = label(n);
        it.dim = wide ? (int)n.space.size() + 1 : 0;
        // Its other tags, which say what else it is.
        int shown = 0, more = 0;
        for (auto& [k, v] : e->tags) {
            if (std::count(tq.want.begin(), tq.want.end(), k)) continue;
            if (shown < 3) it.meta += (shown++ ? " #" : "#") + v;
            else more++;
        }
        if (more) it.meta += " +" + std::to_string(more);
        it.current = note_ && note_->path == n.path;
        it.action = [this, n] { open_note(n); };
        out.push_back(std::move(it));
    }
    std::string terms;
    for (auto& w : tq.want) terms += (terms.empty() ? "#" : " #") + w;
    for (auto& s : tq.skip) terms += (terms.empty() ? "-#" : " -#") + s;
    palette_.set_empty("no notes with " + terms + elsewhere);
}

bool App::tag_at_caret(std::string* name) const {
    LinkSpan l;
    if (!editing() || !is_note() || !editor_.link_at_caret(l) || !l.tag) return false;
    if (name) *name = l.target;
    return true;
}

// Every tag, in every space, listed to pick one: tags mean the same
// everywhere.
void App::pick_tag_rename() {
    std::string at;
    if (tag_at_caret(&at)) { open_tag_rename(at); return; }
    tags_now();
    TagTally all = tags_.tally();
    if (all.empty()) { status("no tags yet; write #name in a note to add one"); return; }
    std::vector<PaletteItem> items;
    for (auto& [k, t] : all) {
        PaletteItem it;
        it.label = "#" + t.name;
        it.meta = count_of(t.notes, "note");
        it.action = [this, name = t.name] { open_tag_rename(name); };
        items.push_back(std::move(it));
    }
    std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return natural_less(a.label, b.label); });
    palette_.pick("rename tag: ", std::move(items));
}

// Renaming a tag: its name in the status line becomes editable, and enter
// asks, in the palette, before every note that has it changes.
void App::open_tag_rename(const std::string& tag) {
    tags_now();
    auto bare = [](std::string v) {
        while (!v.empty() && v[0] == '#') v.erase(0, 1);
        return v;
    };
    BarPrompt p;
    p.lead = {{"rename tag #", &Theme::dim}};
    p.problem = [tag, bare](const std::string& text) -> std::string {
        std::string v = bare(text);
        return v == tag ? std::string() : check_tag(v);
    };
    p.hint = [this, tag, bare](const std::string& text) {
        std::string v = bare(text);
        std::string in = count_of(tags_.notes_with(tag), "note");
        bool merge = !iequals(v, tag) && tags_.notes_with(v) > 0;
        return merge ? "enter to merge into #" + v + ", in " + in : "enter to rename, in " + in;
    };
    p.submit = [this, tag, bare](const std::string& text) {
        std::string v = bare(text);
        if (v != tag) confirm_tag_rename(tag, v);
        return std::string();
    };
    open_prompt(std::move(p), tag);
}

void App::confirm_tag_rename(const std::string& from, const std::string& to) {
    int n = tags_.notes_with(from);
    bool merge = !iequals(from, to) && tags_.notes_with(to) > 0;
    std::string in = count_of(n, "note");
    confirm(merge ? "merge tags? " : "rename tag? ", merge ? "merge #" + from + " into #" + to : "rename #" + from + " to #" + to,
            in + ", every space", [this, from, to] { apply_tag_rename(from, to); });
}

// Every note in every space that has #from gets #to instead. The open note
// changes in the editor, as one undo step; the rest are written in place.
void App::apply_tag_rename(const std::string& from, const std::string& to) {
    refresh_tags(true);
    bool merge = !iequals(from, to) && tags_.notes_with(to) > 0;
    std::vector<NoteRef> notes;
    for (auto& [path, e] : tags_.notes())
        if (e.tags.count(lower(from)) && !(is_note() && e.note.path == note_->path)) notes.push_back(e.note);
    int changed = 0, failed = 0;
    std::string why;
    for (auto& n : notes) {
        std::string err;
        switch (rewrite_lines(n.path, [&](auto& l) { return rename_tag(l, from, to) > 0; }, &err)) {
            case Rewrite::Unchanged: break;
            case Rewrite::Written: changed++; break;
            case Rewrite::Unreadable: failed++; break;
            case Rewrite::Unwritable: failed++; why = err; break;
        }
    }
    if (is_note()) {
        std::vector<std::string> lines = editor_.lines();
        TextPos c = editor_.cursor();
        if (rename_tag(lines, from, to, &c)) {
            editor_.rewrite(lines, c);
            save();
            changed++;
        }
        tags_.set(*note_, editor_.lines());
    }
    refresh_tags(true);
    std::string done = (merge ? "merged #" + from + " into #" : "renamed #" + from + " to #") + to + " in " + count_of(changed, "note");
    if (failed) status(done + "; " + std::to_string(failed) + " couldn't be changed" + (why.empty() ? "" : ": " + why), true);
    else status(done);
}
