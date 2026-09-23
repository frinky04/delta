#pragma once
// Spaces are folders directly under the notes root; notes are the .md files
// inside them. No nesting: sub-folders of a space are ignored, and folders
// starting with '.' (like .trash) are not spaces.
#include <ctime>
#include <string>
#include <vector>

#include "platform.h"

struct NoteRef {
    std::string space;
    std::string name;   // file name without .md
    fs::path path;
    std::time_t mtime = 0;
};

class Store {
public:
    void set_root(const fs::path& root) { root_ = root; }
    const fs::path& root() const { return root_; }

    std::vector<std::string> spaces() const;
    std::vector<NoteRef> notes(const std::string& space) const;  // newest first
    std::vector<NoteRef> all_notes() const;
    bool exists(const std::string& space, const std::string& name) const;
    NoteRef ref(const std::string& space, const std::string& name) const;
    int note_count(const std::string& space) const;

    // Each returns an empty string on success, or what went wrong.
    std::string ensure_space(const std::string& space) const;
    std::string create_note(const std::string& space, const std::string& name, const std::string& content, NoteRef& out) const;
    std::string rename_note(NoteRef& note, const std::string& name) const;
    std::string move_note(NoteRef& note, const std::string& space) const;
    std::string trash_note(const NoteRef& note) const;  // moves into <root>/.trash
    std::string rename_space(const std::string& from, const std::string& to) const;
    std::string delete_space(const std::string& space) const;  // empty spaces only

    static std::string check_name(const std::string& name, const char* what);

private:
    fs::path root_;
};
