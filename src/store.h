#pragma once
// Spaces are folders directly under the notes root; notes are the .md files
// inside them. No nesting: sub-folders of a space are ignored, and folders
// starting with '.' (like .trash) are not spaces.
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "platform.h"

struct NoteRef {
    std::string space;
    std::string name;   // file name without .md
    fs::path path;
    std::time_t mtime = 0;
};

// Every note's name, by space, for looking names up without the disk.
// Keys are lower-cased (names match case-insensitively); values keep the
// names as written.
struct NoteIndex {
    struct Space {
        std::string name;
        std::map<std::string, std::string> notes;
        bool operator==(const Space&) const = default;
    };
    std::map<std::string, Space> spaces;

    // Does a [[link]]'s space ("" for any) and name lead to a note?
    bool has(const std::string& space, const std::string& name) const;
    bool operator==(const NoteIndex&) const = default;
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
    NoteIndex index() const;

    // Each returns an empty string on success, or what went wrong.
    std::string ensure_space(const std::string& space) const;
    std::string create_note(const std::string& space, const std::string& name, const std::string& content, NoteRef& out) const;
    std::string rename_note(NoteRef& note, const std::string& name) const;
    std::string move_note(NoteRef& note, const std::string& space) const;
    std::string trash_note(const NoteRef& note) const;  // moves into <root>/.trash
    std::string rename_space(const std::string& from, const std::string& to) const;
    std::string delete_space(const std::string& space) const;  // empty spaces only

    static std::string check_name(const std::string& name, const char* what);
    // A [[link]] target as written: "name" or "space/name", any "#heading"
    // after it dropped. `space` is "" when it doesn't name one. False if
    // there's no name left.
    static bool split_target(const std::string& target, std::string& space, std::string& name);

private:
    fs::path root_;
};
