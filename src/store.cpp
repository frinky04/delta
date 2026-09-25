#include "store.h"

#include <algorithm>
#include <chrono>

#include "util.h"

static std::time_t to_time_t(fs::file_time_type ft) {
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::system_clock::to_time_t(sys);
}

static bool is_note_file(const fs::directory_entry& e) {
    std::error_code ec;
    if (!e.is_regular_file(ec)) return false;
    return lower(path_u8(e.path().extension())) == ".md";
}

static NoteRef note_at(const fs::directory_entry& e, const std::string& space) {
    NoteRef n;
    n.space = space;
    n.name = path_u8(e.path().stem());
    n.path = e.path();
    std::error_code ec;
    auto t = e.last_write_time(ec);
    if (!ec) n.mtime = to_time_t(t);
    return n;
}

static void newest_first(std::vector<NoteRef>& notes) {
    std::sort(notes.begin(), notes.end(), [](const NoteRef& a, const NoteRef& b) { return a.mtime > b.mtime; });
}

std::string Store::check_name(const std::string& raw, const char* what) {
    std::string name = trim(raw);
    if (name.empty()) return std::string("type a ") + what + " name";
    if (name[0] == '.') return std::string("a ") + what + " name can't start with a dot";
    if (name.back() == '.') return std::string("a ") + what + " name can't end with a dot";
    for (char c : name)
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || (unsigned char)c < 32)
            return std::string("a ") + what + " name can't contain " + (c < 32 ? std::string("control characters") : std::string(1, c));
    if (name.size() > 200) return std::string("that ") + what + " name is too long";
    return {};
}

std::vector<std::string> Store::spaces() const {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_, ec)) {
        std::error_code ec2;
        if (!e.is_directory(ec2)) continue;
        std::string name = path_u8(e.path().filename());
        if (name.empty() || name[0] == '.') continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    return out;
}

std::vector<NoteRef> Store::notes(const std::string& space) const {
    std::vector<NoteRef> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / u8path(space), ec))
        if (is_note_file(e)) out.push_back(note_at(e, space));
    newest_first(out);
    return out;
}

std::vector<NoteRef> Store::all_notes() const {
    std::vector<NoteRef> out;
    for (auto& s : spaces()) {
        auto n = notes(s);
        out.insert(out.end(), n.begin(), n.end());
    }
    newest_first(out);
    return out;
}

int Store::note_count(const std::string& space) const {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root_ / u8path(space), ec))
        if (is_note_file(e)) n++;
    return n;
}

NoteIndex Store::index() const {
    NoteIndex ix;
    for (auto& s : spaces()) {
        NoteIndex::Space& sp = ix.spaces[lower(s)];
        sp.name = s;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(root_ / u8path(s), ec)) {
            if (!is_note_file(e)) continue;
            std::string n = path_u8(e.path().stem());
            sp.notes[lower(n)] = n;
        }
    }
    return ix;
}

bool NoteIndex::has(const std::string& space, const std::string& name) const {
    std::string n = lower(name);
    if (!space.empty()) {
        auto s = spaces.find(lower(space));
        return s != spaces.end() && s->second.notes.count(n);
    }
    for (auto& [k, s] : spaces)
        if (s.notes.count(n)) return true;
    return false;
}

bool NoteIndex::resolve(const std::string& from, const std::string& space, const std::string& name,
                        std::string& out_space, std::string& out_name) const {
    std::string n = lower(name);
    auto in = [&](const Space& s) {
        auto it = s.notes.find(n);
        if (it == s.notes.end()) return false;
        out_space = s.name;
        out_name = it->second;
        return true;
    };
    if (!space.empty()) {
        auto s = spaces.find(lower(space));
        return s != spaces.end() && in(s->second);
    }
    if (auto s = spaces.find(lower(from)); s != spaces.end() && in(s->second)) return true;
    for (auto& [k, s] : spaces)
        if (in(s)) return true;
    return false;
}

bool Store::split_target(const std::string& target, std::string& space, std::string& name) {
    std::string t = trim(target);
    if (auto h = t.find('#'); h != std::string::npos) t = trim(t.substr(0, h));
    space.clear();
    name = t;
    if (auto slash = t.find('/'); slash != std::string::npos) {
        space = trim(t.substr(0, slash));
        name = trim(t.substr(slash + 1));
    }
    return !name.empty();
}

NoteRef Store::ref(const std::string& space, const std::string& name) const {
    NoteRef n;
    n.space = space;
    n.name = name;
    n.path = root_ / u8path(space) / u8path(name + ".md");
    if (auto t = mtime_of(n.path); t != fs::file_time_type{}) n.mtime = to_time_t(t);
    return n;
}

bool Store::exists(const std::string& space, const std::string& name) const {
    std::error_code ec;
    return fs::exists(root_ / u8path(space) / u8path(name + ".md"), ec);
}

std::string Store::ensure_space(const std::string& space) const {
    if (auto e = check_name(space, "space"); !e.empty()) return e;
    std::error_code ec;
    fs::create_directories(root_ / u8path(trim(space)), ec);
    if (ec) return "couldn't create space: " + plain_msg(ec.message());
    return {};
}

std::string Store::create_note(const std::string& space, const std::string& raw, const std::string& content, NoteRef& out) const {
    std::string name = trim(raw);
    if (auto e = check_name(name, "note"); !e.empty()) return e;
    if (auto e = ensure_space(space); !e.empty()) return e;
    if (exists(space, name)) return "\"" + name + "\" already exists in " + space;
    NoteRef n = ref(space, name);
    std::string err;
    if (!atomic_write(n.path, content, &err)) return "couldn't create note: " + err;
    out = ref(space, name);
    return {};
}

std::string Store::rename_note(NoteRef& note, const std::string& raw) const {
    std::string name = trim(raw);
    if (auto e = check_name(name, "note"); !e.empty()) return e;
    if (name == note.name) return {};
    // Allow case-only renames (same file on Windows).
    if (!iequals(name, note.name) && exists(note.space, name)) return "\"" + name + "\" already exists in " + note.space;
    NoteRef to = ref(note.space, name);
    std::error_code ec;
    fs::rename(note.path, to.path, ec);
    if (ec) return "couldn't rename: " + plain_msg(ec.message());
    note = ref(note.space, name);
    return {};
}

std::string Store::move_note(NoteRef& note, const std::string& space) const {
    if (auto e = ensure_space(space); !e.empty()) return e;
    if (exists(space, note.name)) return "\"" + note.name + "\" already exists in " + space;
    NoteRef to = ref(space, note.name);
    std::error_code ec;
    fs::rename(note.path, to.path, ec);
    if (ec) return "couldn't move: " + plain_msg(ec.message());
    note = ref(space, note.name);
    return {};
}

std::string Store::trash_note(const NoteRef& note) const {
    std::error_code ec;
    fs::path dir = root_ / ".trash" / u8path(note.space);
    fs::create_directories(dir, ec);
    fs::path to = dir / u8path(note.name + ".md");
    for (int i = 2; fs::exists(to, ec); i++) to = dir / u8path(note.name + " (" + std::to_string(i) + ").md");
    fs::rename(note.path, to, ec);
    if (ec) return "couldn't delete: " + plain_msg(ec.message());
    // Stamped with when it was deleted, which is what the trash is sorted by.
    fs::last_write_time(to, fs::file_time_type::clock::now(), ec);
    return {};
}

std::vector<NoteRef> Store::trashed() const {
    std::vector<NoteRef> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(root_ / ".trash", ec)) {
        std::error_code ec2;
        if (!d.is_directory(ec2)) continue;
        std::string space = path_u8(d.path().filename());
        for (auto& e : fs::directory_iterator(d.path(), ec2))
            if (is_note_file(e)) out.push_back(note_at(e, space));
    }
    newest_first(out);
    return out;
}

std::string Store::restore_note(const NoteRef& trashed, NoteRef& out) const {
    if (auto e = ensure_space(trashed.space); !e.empty()) return e;
    std::string name = trashed.name;
    for (int i = 2; exists(trashed.space, name); i++) name = trashed.name + " (" + std::to_string(i) + ")";
    NoteRef to = ref(trashed.space, name);
    std::error_code ec;
    fs::rename(trashed.path, to.path, ec);
    if (ec) return "couldn't restore: " + plain_msg(ec.message());
    out = ref(trashed.space, name);
    return {};
}

std::string Store::rename_space(const std::string& from, const std::string& raw) const {
    std::string to = trim(raw);
    if (auto e = check_name(to, "space"); !e.empty()) return e;
    std::error_code ec;
    if (!iequals(from, to) && fs::exists(root_ / u8path(to), ec)) return "a space called \"" + to + "\" already exists";
    fs::rename(root_ / u8path(from), root_ / u8path(to), ec);
    if (ec) return "couldn't rename space: " + plain_msg(ec.message());
    return {};
}

std::string Store::delete_space(const std::string& space) const {
    std::error_code ec;
    fs::path dir = root_ / u8path(space);
    if (!fs::is_empty(dir, ec)) return "\"" + space + "\" isn't empty; move or delete its notes first";
    fs::remove(dir, ec);
    if (ec) return "couldn't delete space: " + plain_msg(ec.message());
    return {};
}
