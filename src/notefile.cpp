#include "notefile.h"
#include "textfile.h"
#include "util.h"

namespace {
// The file's text, noting its line endings; '\r' is dropped on the way in.
bool read_note(const fs::path& p, std::string& text, bool& crlf, std::string* err) {
    std::string raw;
    if (!read_file(p, raw)) {
        if (err) *err = "couldn't read " + path_u8(p.filename());
        return false;
    }
    crlf = has_crlf(raw);
    text = join_lines(split_lines(raw));
    return true;
}
}  // namespace

bool NoteFile::load(const fs::path& p, std::string& text, std::string* err) {
    std::string t;
    bool crlf = false;
    if (!read_note(p, t, crlf, err)) return false;
    text = std::move(t);
    path_ = p;
    crlf_ = crlf;
    mtime_ = mtime_of(p);
    conflict_ = false;
    return true;
}

void NoteFile::synced(uint64_t version) { saved_ = version; }

void NoteFile::close() { *this = NoteFile(); }

void NoteFile::moved(const fs::path& p) {
    path_ = p;
    mtime_ = mtime_of(p);
}

std::string NoteFile::encode(const std::string& text) const {
    return crlf_ ? join_lines(split_lines(text), eol_of(true)) : text;
}

NoteFile::Save NoteFile::save(const std::string& text, uint64_t version, bool over_disk, std::string* err) {
    if (!open() || version == saved_) return Save::Clean;
    if (!over_disk && !conflict_) {
        // Changed on disk since we last looked (before a poll saw it).
        auto nm = mtime_of(path_);
        if (nm != fs::file_time_type{} && mtime_ != fs::file_time_type{} && nm != mtime_) {
            mtime_ = nm;
            conflict_ = true;
        }
    }
    if (conflict_ && !over_disk) return Save::Held;
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    if (!atomic_write(path_, encode(text), err)) return Save::Failed;
    saved_ = version;
    mtime_ = mtime_of(path_);
    conflict_ = false;
    return Save::Saved;
}

NoteFile::Poll NoteFile::poll(uint64_t version) {
    if (!open()) return Poll::Same;
    auto nm = mtime_of(path_);
    if (nm == fs::file_time_type{} || nm == mtime_) return Poll::Same;
    mtime_ = nm;
    if (version != saved_) {
        conflict_ = true;
        return Poll::Conflict;
    }
    return Poll::Changed;
}

bool NoteFile::reload(std::string& text, std::string* err) {
    if (!open()) return false;
    std::string t;
    bool crlf = false;
    if (!read_note(path_, t, crlf, err)) return false;
    text = std::move(t);
    crlf_ = crlf;
    mtime_ = mtime_of(path_);
    conflict_ = false;
    return true;
}

std::string NoteFile::keep_beside(const std::string& text, uint64_t version, std::string* err) {
    if (!conflict_ || !open() || version == saved_) {
        conflict_ = false;
        return "";
    }
    conflict_ = false;
    fs::path dir = path_.parent_path(), ext = path_.extension();
    std::string stem = path_u8(path_.stem()) + " (yours)", name = stem;
    std::error_code ec;
    for (int i = 2; fs::exists(dir / u8path(name + path_u8(ext)), ec); i++) name = stem + " " + std::to_string(i);
    // On failure the edits stay unsaved here, and out of conflict.
    if (!atomic_write(dir / u8path(name + path_u8(ext)), encode(text), err)) return "";
    saved_ = version;  // nothing left to save over the other version
    return name;
}
