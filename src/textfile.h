#pragma once
// A text file as lines, with the line endings it came with, so what's
// written back keeps them.
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "platform.h"
#include "util.h"

inline bool has_crlf(std::string_view text) { return text.find("\r\n") != std::string_view::npos; }
inline const char* eol_of(bool crlf) { return crlf ? "\r\n" : "\n"; }

struct TextFile {
    std::vector<std::string> lines{""};
    bool crlf = false;
    // False if `p` can't be read; then it's one empty line, as a new file is.
    bool load(const fs::path& p);
    std::string text() const { return join_lines(lines, eol_of(crlf)); }
    bool save(const fs::path& p, std::string* err) const;  // atomic_write of text()
};

enum class Rewrite { Unchanged, Written, Unreadable, Unwritable };
// `p` as lines through `edit` (true: it changed them) and back with the
// file's own line endings. Unchanged writes nothing.
Rewrite rewrite_lines(const fs::path& p, const std::function<bool(std::vector<std::string>&)>& edit,
                      std::string* err = nullptr);
