// Docs from outside the notes: files and folders dropped on the window (or
// the path delta was started with) routed to where they go, visiting a doc
// (reading it without keeping it), and keeping or importing docs as notes,
// with the images they show brought along. What's planned without the disk
// is in import.h.
#include <algorithm>
#include <map>
#include <set>

#include "app.h"
#include "import.h"
#include "textfile.h"
#include "util.h"

struct App::ImportTally {
    int notes = 0;      // made
    int renamed = 0;    // under another name, theirs being taken
    int images = 0;     // brought into the image store
    int missing = 0;    // images referred to but not found (or not images)
    int failed = 0;     // docs that couldn't be read or written
    std::string first_name, error;
};

// Drops come in as the frame starts (see on_drop).
void App::handle_drops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (auto& drop : drops) {
        std::vector<fs::path> paths;
        for (auto& p : drop.paths) paths.push_back(u8path(p));
        open_paths(paths, drop.at);
    }
}

// Where each dropped thing goes: images into the open note, as ever; one doc
// alone visits; several docs, or a folder, are imported into a space picked
// in the palette. A note's own file, or a space's folder, just opens.
void App::open_paths(const std::vector<fs::path>& in, ImVec2 at) {
    std::vector<fs::path> paths;
    for (auto& p : in) {
        std::error_code ec;
        fs::path a = fs::absolute(p, ec);
        paths.push_back((ec ? p : a).lexically_normal());
    }
    DropPlan plan = plan_drop(paths, [](const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p, ec);
    });
    if (!plan.images.empty()) drop_images(plan.images, at);

    // Already in the notes: a space's folder, or a note in one.
    auto in_notes = [this](const fs::path& dir) {
        std::error_code ec;
        std::string name = path_u8(dir.filename());
        return !name.empty() && name[0] != '.' && fs::equivalent(dir.parent_path(), store_.root(), ec);
    };
    if (plan.folders.size() == 1 && plan.docs.empty() && in_notes(plan.folders[0])) {
        switch_space(path_u8(plan.folders[0].filename()));
        return;
    }
    if (plan.docs.size() == 1 && plan.folders.empty() && lower(path_u8(plan.docs[0].extension())) == ".md" &&
        in_notes(plan.docs[0].parent_path())) {
        open_note(store_.ref(path_u8(plan.docs[0].parent_path().filename()), path_u8(plan.docs[0].stem())));
        return;
    }

    int others = (int)plan.others.size();
    if (plan.docs.empty() && plan.folders.empty()) {
        if (others == 1) status("delta takes .md and .txt docs and images; " + path_u8(plan.others[0].filename()) + " is neither");
        else if (others) status(std::to_string(others) + " files skipped; delta takes .md and .txt docs and images");
        return;
    }
    if (plan.docs.size() == 1 && plan.folders.empty()) {
        visit(plan.docs[0]);
        return;
    }

    // Several: the docs dropped and each folder's own (not its sub-folders').
    std::vector<fs::path> docs = plan.docs;
    int left = others;
    for (auto& dir : plan.folders) {
        std::vector<FolderEntry> entries;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            std::error_code ec2;
            entries.push_back({path_u8(e.path().filename()), e.is_directory(ec2)});
        }
        if (ec) { status("couldn't read " + path_u8(dir.filename()) + ": " + plain_msg(ec.message()), true); return; }
        FolderPlan fp = plan_folder(entries);
        for (auto& name : fp.docs) docs.push_back(dir / u8path(name));
        left += (int)fp.left.size();
    }
    if (docs.empty()) {
        std::string where = plan.folders.size() == 1 ? path_u8(plan.folders[0].filename()) : std::string("those folders");
        status("no .md or .txt files in " + where + "; only a folder's top level is imported");
        return;
    }
    pick_import(std::move(docs), plan.folders.size() == 1 && plan.docs.empty() ? plan.folders[0] : fs::path(), left);
}

// A doc from outside, read in place: nothing of it is kept unless you keep it.
void App::visit(const fs::path& file) {
    if (palette_.is_open()) palette_.close();
    NoteRef r;
    r.space = "visiting";
    r.name = path_u8(file.filename());
    r.path = file;
    r.visit = true;
    open_note(r);
    if (note_ && note_->visit && note_->path == file) {
        std::string keep = key_hint("note.keep", "keep it as a note");
        status("visiting " + r.name + "; read only" + (keep.empty() ? "" : "; " + keep));
    }
}

void App::keep_note() {
    if (is_manual()) { status("the manual stays built in; open a note to keep your own"); return; }
    if (!is_visiting()) { status("nothing visiting to keep; drop a .md or .txt file on the window to read it"); return; }
    pick_import({note_->path}, fs::path(), 0);
}

void App::pick_import(std::vector<fs::path> docs, const fs::path& folder, int left) {
    bool keep = is_visiting() && docs.size() == 1 && docs[0] == note_->path;
    auto row = [=, this](const std::string& space, bool make) {
        PaletteItem it;
        it.action = [this, docs, space, left, keep] { import_docs(docs, space, left, keep); };
        it.label = make ? "+ new space \"" + space + "\"" : space;
        return it;
    };
    std::vector<PaletteItem> items;
    // A folder dropped alone can be a space of its own, named after it.
    if (!folder.empty()) {
        std::string want = doc_note_name(folder.filename());
        std::string name = free_name(want, [this](const std::string& n) {
            std::error_code ec;
            return fs::exists(store_.root() / u8path(n), ec);
        });
        PaletteItem it = row(name, true);
        if (name != want) it.meta = want + " is taken";
        items.push_back(std::move(it));
    }
    // This space first, then the rest.
    std::vector<std::string> spaces = store_.spaces();
    std::stable_partition(spaces.begin(), spaces.end(), [this](const std::string& s) { return s == space_; });
    for (auto& sp : spaces) {
        PaletteItem it = row(sp, false);
        it.meta = sp == space_ ? "current" : count_of(store_.note_count(sp), "note");
        items.push_back(std::move(it));
    }
    Palette::PickOptions opts;
    // Typed, a space that isn't there yet: made for them.
    opts.typed = [this, row](const std::string& text, PaletteItem& it) {
        std::string t = trim(text);
        if (t.empty() || !Store::check_name(t, "space").empty()) return false;
        for (auto& sp : store_.spaces())
            if (iequals(sp, t)) return false;
        it = row(t, true);
        return true;
    };
    std::string what = keep ? "keep " + path_u8(docs[0].filename()) + " in: "
                     : "import " + count_of((int)docs.size(), "file") + (folder.empty() ? "" : " from " + path_u8(folder.filename())) + " into: ";
    palette_.pick(what, std::move(items), std::move(opts));
}

void App::import_docs(const std::vector<fs::path>& docs, const std::string& space, int left, bool keep) {
    if (auto e = store_.ensure_space(space); !e.empty()) { status(e, true); return; }
    ImportTally t;
    NoteRef first;
    for (auto& d : docs) {
        NoteRef n;
        if (!import_doc(d, space, t, n)) continue;
        if (t.notes == 1) first = n;
    }
    space_count_ = -1;
    note_index_ = store_.index();
    if (t.notes == 0) {
        status("couldn't import " + (docs.size() == 1 ? path_u8(docs[0].filename()) : std::string("those files")) +
                   (t.error.empty() ? "" : ": " + t.error), true);
        return;
    }
    open_note(first);

    // What happened, then what didn't quite.
    std::string msg;
    if (keep) msg = "kept as " + first.name + " in " + space;
    else msg = "imported " + count_of(t.notes, "note") + " into " + space;
    if (t.renamed) msg += keep ? std::string("; ") + t.first_name + " was taken" : "; " + std::to_string(t.renamed) + " renamed, their names taken";
    if (t.images) msg += "; " + count_of(t.images, "image") + " brought along";
    if (t.missing) msg += "; " + count_of(t.missing, "image") + " not found";
    if (t.failed) msg += "; " + std::to_string(t.failed) + " failed: " + t.error;
    if (left) msg += "; " + std::to_string(left) + " skipped";
    status(msg, t.failed > 0);
}

// One doc into `space` as a note: its text as it is (front matter and all),
// under its own name or the next free one, and the images it shows from files
// beside it copied into the image store, their links pointing there.
bool App::import_doc(const fs::path& file, const std::string& space, ImportTally& t, NoteRef& out) {
    std::string text;
    if (!read_file(file, text)) {
        t.failed++;
        if (t.error.empty()) t.error = "couldn't read " + path_u8(file.filename());
        return false;
    }
    std::vector<std::string> lines = split_lines(text);
    std::vector<DocImage> images = local_images(lines);
    std::map<std::string, std::string> to;
    std::set<std::string> missing;
    for (auto& im : images) {
        if (to.count(im.src) || missing.count(im.src)) continue;
        fs::path p = resolve_image(file.parent_path(), im.src);
        std::string bytes, err, link;
        if (!p.empty() && read_file(p, bytes)) link = store_image(bytes, err);
        if (link.empty()) missing.insert(im.src);
        else to[im.src] = link;
    }
    if (!to.empty()) {
        rewrite_images(lines, images, to);
        text = join_lines(lines, eol_of(has_crlf(text)));
    }
    std::string want = doc_note_name(file);
    std::string name = free_name(want, [&](const std::string& n) { return store_.exists(space, n); });
    if (auto e = store_.create_note(space, name, text, out); !e.empty()) {
        t.failed++;
        if (t.error.empty()) t.error = e;
        return false;
    }
    if (name != want) {
        if (!t.renamed) t.first_name = want;
        t.renamed++;
    }
    t.notes++;
    t.images += (int)to.size();
    t.missing += (int)missing.size();
    return true;
}
