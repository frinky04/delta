#include "nav.h"

#include <algorithm>

#include "util.h"

static constexpr size_t kMaxPlaces = 200, kMaxRecent = 200;

static bool somewhere(const Place& p) { return !p.note.path.empty(); }
static bool same_line(const Place& a, const Place& b) { return a.note.path == b.note.path && a.caret.line == b.caret.line; }

void History::jumped(const Place& from, const Place& to, Kind kind) {
    if (!somewhere(to) || same_line(from, to)) return;
    // Another step of the same run, from where the last one landed: the
    // run's end moves on, and back still goes to where it started.
    if (kind != Other && kind == run_ && pos_ >= 0 && from.note.path == run_end_.note.path && from.caret == run_end_.caret) {
        places_[pos_] = to;
        run_end_ = to;
        return;
    }
    places_.resize(pos_ + 1);  // as in a browser, going somewhere new ends forward
    if (somewhere(from)) {
        if (pos_ >= 0 && places_[pos_].note.path == from.note.path) places_[pos_] = from;
        else places_.push_back(from);  // a note opened some other way
    }
    places_.push_back(to);
    if (places_.size() > kMaxPlaces) places_.erase(places_.begin(), places_.end() - kMaxPlaces);
    pos_ = (int)places_.size() - 1;
    run_ = kind;
    run_end_ = to;
}

const Place* History::step(int dir, const Place& here, const std::function<bool(const Place&)>& exists) {
    if (pos_ < 0) return nullptr;
    run_ = Other;
    if (somewhere(here)) {
        if (places_[pos_].note.path == here.note.path) {
            places_[pos_] = here;
        } else {
            // Somewhere the history didn't see you go: that's where you are.
            places_.resize(pos_ + 1);
            places_.push_back(here);
            pos_++;
        }
    }
    int p = pos_ + dir;
    while (p >= 0 && p < (int)places_.size()) {
        if (exists(places_[p])) {
            pos_ = p;
            return &places_[p];
        }
        places_.erase(places_.begin() + p);
        if (dir < 0) { p--; pos_--; }
    }
    return nullptr;
}

void History::update(const std::function<void(NoteRef&)>& fn) {
    for (auto& p : places_) fn(p.note);
    fn(run_end_.note);
}

void History::forget(const fs::path& note) {
    std::vector<Place> kept;
    int pos = -1;
    for (int i = 0; i < (int)places_.size(); i++) {
        // Its places go, and the ones either side may now be the same.
        bool drop = places_[i].note.path == note || (!kept.empty() && same_line(kept.back(), places_[i]));
        if (!drop) kept.push_back(places_[i]);
        if (i == pos_) pos = (int)kept.size() - 1;
    }
    places_ = std::move(kept);
    pos_ = places_.empty() ? -1 : std::max(pos, 0);
    run_ = Other;
}

void Recent::visit(const NoteRef& n) {
    forget(n.path);
    notes_.insert(notes_.begin(), n);
    if (notes_.size() > kMaxRecent) notes_.resize(kMaxRecent);
}

void Recent::update(const std::function<void(NoteRef&)>& fn) {
    for (auto& n : notes_) fn(n);
}

void Recent::forget(const fs::path& note) {
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [&](const NoteRef& n) { return n.path == note; }), notes_.end());
}

bool parse_line_target(const std::string& q, int origin, const std::vector<std::string>& lines, TextPos& to) {
    if (q.empty()) return false;
    size_t i = 0;
    int sign = 0;
    if (q[0] == '+' || q[0] == '-') { sign = q[0] == '+' ? 1 : -1; i = 1; }
    auto number = [&](long& n) {
        size_t start = i;
        n = 0;
        while (i < q.size() && q[i] >= '0' && q[i] <= '9' && n < 10000000) n = n * 10 + (q[i++] - '0');
        return i > start;
    };
    long line = 0, col = 1;
    if (!number(line)) return false;
    if (i < q.size() && q[i] == ':') {
        i++;
        if (i < q.size() && !number(col)) return false;
    }
    if (i != q.size()) return false;
    long target = sign ? origin + sign * line : line - 1;
    if (!sign && line < 1) return false;
    to.line = (int)std::clamp<long>(target, 0, (long)lines.size() - 1);
    // Columns count characters, like the status bar's.
    const std::string& s = lines[to.line];
    int b = 0;
    for (long c = 1; c < col && b < (int)s.size(); c++) b = utf8::next(s, b);
    to.col = b;
    return true;
}
