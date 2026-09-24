#pragma once
// #tags: finding them in a note's lines, which notes have which, the
// palette's tag queries, and renaming a tag in every note.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "editor.h"
#include "store.h"

// A tag in a note: its line, the byte range of "#name", and the name as
// written.
struct TagUse {
    int line, col0, col1;
    std::string name;
};
// Every tag in these lines, in order. Code is skipped.
std::vector<TagUse> find_tags(const std::vector<std::string>& lines);

// A note's tags: each lower-cased name (tags match whatever their case), to
// the name as first written.
using TagSet = std::map<std::string, std::string>;
TagSet tags_of(const std::vector<std::string>& lines);

// Why `name` (without its #) can't be a tag, or "" if it can.
std::string check_tag(const std::string& name);

// The tag at a place in a line: where its word begins (just after the #) if
// `col` is at the end of one being written ("#pro|"), else -1. For completing.
int tag_word_start(const std::string& line, int col, const std::vector<Style>& st);

// Every #from (any case) in these lines becomes #to. The caret, if given,
// stays with the text it was in. Returns how many were changed.
int rename_tag(std::vector<std::string>& lines, const std::string& from, const std::string& to, TextPos* caret = nullptr);

// A tag query as typed in the palette after its #: tags a note must have
// ("#a #b"), tags it mustn't ("-#done"), and the term still being typed (the
// last, until a space finishes it). The # before each term is optional.
struct TagQuery {
    std::vector<std::string> want, skip;  // finished terms, lower-cased
    bool typing = false;                  // the last term isn't finished
    bool typing_skip = false;             // ...and is a "-" one
    std::string word;                     // ...as typed so far, without "-" or "#"
};
TagQuery parse_tag_query(const std::string& text);
// Whether a note with these tags answers the finished terms.
bool tag_query_match(const TagQuery& q, const TagSet& tags);
// The query with the term being typed finished as `name` (and a space, for
// the next). The first term goes without its #: the prompt has it.
std::string tag_query_fill(const std::string& text, const std::string& name);

// Which notes have which tags. Notes are read once, then again only when
// they change on disk.
class TagIndex {
public:
    struct Entry {
        NoteRef note;
        TagSet tags;
        int64_t stamp = 0;  // the file's time and size when read
    };
    // Reads the notes that are new or changed since last time, and forgets
    // the ones that are gone.
    void refresh(const std::vector<NoteRef>& notes);
    // A note's tags as they are in the editor (ahead of the disk until it's
    // saved).
    void set(const NoteRef& n, const std::vector<std::string>& lines);
    const std::map<std::string, Entry>& notes() const { return notes_; }  // by path

private:
    std::map<std::string, Entry> notes_;
};
