#pragma once
// Getting around between notes: where you've been (back and forward, like a
// browser) and which notes you've had open lately (the switcher, ctrl+tab).
#include <functional>
#include <string>
#include <vector>

#include "editor.h"
#include "store.h"

// A place in a note, as you left it. No note (an empty path): nowhere.
struct Place {
    NoteRef note;
    TextPos caret;
    float scroll = 0;
};

// Back and forward go through places, not just notes. Big jumps add one:
// opening a note, following a link, going to a heading, a line, a problem
// or a search result. Typing and moving the caret about don't. Each place is
// kept as you left it, so going back returns to where you were in that note.
class History {
public:
    // What kind of jump: a run of stepping jumps of one kind (next heading,
    // next heading...) with nothing done in between counts as one, so back
    // goes to where the run started.
    enum Kind { Other, Heading, Problem, Misspelling };

    // A jump from `from` landed at `to`. Nothing is added if it didn't go
    // anywhere (the same line of the same note).
    void jumped(const Place& from, const Place& to, Kind kind = Other);
    // Back (-1) or forward (+1) from `here`, which is kept as the place
    // you're leaving. Places whose note is gone (`exists` says) are dropped
    // on the way. Returns where to go, or null at the end.
    const Place* step(int dir, const Place& here, const std::function<bool(const Place&)>& exists);
    // A note renamed or moved: `fn` updates each place's note.
    void update(const std::function<void(NoteRef&)>& fn);
    void forget(const fs::path& note);    // deleted

    int size() const { return (int)places_.size(); }

private:
    std::vector<Place> places_;
    int pos_ = -1;
    Kind run_ = Other;       // the last jump's kind, while you're still where it landed
    Place run_end_;
};

// Notes by when you last opened them, the latest first.
class Recent {
public:
    void visit(const NoteRef& n);
    void update(const std::function<void(NoteRef&)>& fn);
    void forget(const fs::path& note);
    const std::vector<NoteRef>& list() const { return notes_; }

private:
    std::vector<NoteRef> notes_;
};

// Go to line: where "42", "42:10" (line and column, in characters) or
// "+5" / "-5" (from line `origin`) lands in `lines`. False if it isn't one.
bool parse_line_target(const std::string& q, int origin, const std::vector<std::string>& lines, TextPos& to);
