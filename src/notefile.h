#pragma once
// The open note's file: its text as read and written (the line endings it
// came with kept), and how the editor's copy stands against the disk. Once
// the file changes under unsaved edits (a conflict) nothing saves over it
// until a save over it or a reload picks a version; leaving instead keeps
// the edits in a copy beside it.
#include <cstdint>
#include <string>
#include "platform.h"

class NoteFile {
public:
    // Reads `p` for the editor: `text` with '\n' endings, the file's noted.
    // On failure nothing here changes.
    bool load(const fs::path& p, std::string& text, std::string* err);
    // The editor holds the text as `version`: that's what the disk has.
    void synced(uint64_t version);
    void close();
    // Renamed or moved: the same content, at `p` now.
    void moved(const fs::path& p);

    bool open() const { return !path_.empty(); }
    const fs::path& path() const { return path_; }
    bool crlf() const { return crlf_; }
    bool conflict() const { return conflict_; }
    bool dirty(uint64_t version) const { return open() && version != saved_; }
    // `text` (editor form) as the file gets it.
    std::string encode(const std::string& text) const;

    enum class Save { Clean, Saved, Held, Failed };
    // Writes `text` as `version`. Clean: nothing to write. Held: the file
    // changed under these edits (a conflict now) and `over_disk` wasn't
    // given. Failed: `err` says why.
    Save save(const std::string& text, uint64_t version, bool over_disk, std::string* err);

    enum class Poll { Same, Changed, Conflict };
    // Looks at the disk. Changed: rewritten with nothing unsaved here (reload
    // it). Conflict: rewritten under unsaved edits (saving is held). A file
    // that can't be read is Same.
    Poll poll(uint64_t version);
    // The file read again, any conflict left behind; then synced().
    bool reload(std::string& text, std::string* err);
    // Leaving a conflict: `text` (yours, at `version`) goes to
    // "<name> (yours).md", numbered if taken, beside the file, which keeps
    // the disk's version. Returns the copy's name; "" when there was no
    // conflict or nothing unsaved, or on failure (`err` set).
    std::string keep_beside(const std::string& text, uint64_t version, std::string* err);

private:
    fs::path path_;
    bool crlf_ = false;
    fs::file_time_type mtime_{};
    uint64_t saved_ = 0;
    bool conflict_ = false;
};
