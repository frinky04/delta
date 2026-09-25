#pragma once
// Docs from outside the notes: which dropped files and folders are docs,
// what a doc is called once it's a note, and the images it brings along.
// Only the planning is here; App (app_import.cpp) reads and writes the files.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "platform.h"

// A doc delta reads: a .md or .txt file (by its name; case aside).
bool is_doc_file(const fs::path& p);
// An image file delta can keep, by its name (.png, .jpg, .gif...).
bool is_image_file(const fs::path& p);

// What a drop holds, in the order dropped: images (for the open note), docs,
// folders, and the rest (neither: not taken). `is_dir` asks the disk.
struct DropPlan {
    std::vector<fs::path> images, docs, folders, others;
};
DropPlan plan_drop(const std::vector<fs::path>& paths, const std::function<bool(const fs::path&)>& is_dir);

// A folder's top level, as listed: which files are docs to import and which
// other files are left behind, each in name order. Spaces don't nest, so
// sub-folders are passed over (their images can still come along with a
// doc), and so are hidden entries (".git").
struct FolderEntry {
    std::string name;
    bool dir = false;
};
struct FolderPlan {
    std::vector<std::string> docs;
    std::vector<std::string> left;  // files not imported
};
FolderPlan plan_folder(const std::vector<FolderEntry>& entries);

// The note name a doc's file name gives: its stem, without the dots a note
// name can't start or end with, and "untitled" when nothing's left.
std::string doc_note_name(const fs::path& file);

// `want`, or the first of "want 2", "want 3"... that isn't `taken`.
std::string free_name(const std::string& want, const std::function<bool(const std::string&)>& taken);

// An image a doc shows from a file beside it: ![alt](img/a.png), anywhere in
// a line outside code and front matter. Web addresses, data: and absolute
// paths aren't local, and stay as they are. `at` and `len` are where `src`
// is written in the line.
struct DocImage {
    int line = 0;
    int at = 0, len = 0;
    std::string src;
};
std::vector<DocImage> local_images(const std::vector<std::string>& lines);
// Is `src` a path relative to the doc (not a web address, data: or absolute)?
bool is_local_src(const std::string& src);
// Each image whose src is in `to` points there instead; the rest of the
// text is left exactly as it was.
void rewrite_images(std::vector<std::string>& lines, const std::vector<DocImage>& images,
                    const std::map<std::string, std::string>& to);
