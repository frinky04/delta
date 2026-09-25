#include "textfile.h"

bool TextFile::load(const fs::path& p) {
    std::string text;
    bool ok = read_file(p, text);
    if (!ok) text.clear();
    crlf = has_crlf(text);
    lines = split_lines(text);
    return ok;
}

bool TextFile::save(const fs::path& p, std::string* err) const { return atomic_write(p, text(), err); }

Rewrite rewrite_lines(const fs::path& p, const std::function<bool(std::vector<std::string>&)>& edit, std::string* err) {
    TextFile f;
    if (!f.load(p)) return Rewrite::Unreadable;
    if (!edit(f.lines)) return Rewrite::Unchanged;
    return f.save(p, err) ? Rewrite::Written : Rewrite::Unwritable;
}
