#pragma once
// Finding a command by what you'd call it. The command table itself lives
// in App (each command runs App code); what's here is how the palette
// matches a query against a command, and the extra words some commands are
// found by.
#include <string>
#include <vector>

// Words a command is also found by, where people's word for it isn't in
// its title ("backlinks" for "show notes that link here"). Space-separated;
// empty for most.
const char* command_keywords(const std::string& id);

// Every word of the query must be found, in any order: at the start of a
// word of the title (best), anywhere in the title, or at the start of a
// word of the id ("note" in note.rename) or of the keywords. A query that
// fails that as a single word falls back to letters in order across the
// title ("tgrm"), below any word match. `hl` gets the title's matched
// bytes; `via` the id word or keyword a word was found by when the title
// didn't have it, to show beside the row.
bool match_command(const std::string& query, const std::string& title, const std::string& id, const std::string& keywords,
                   int& score, std::vector<int>* hl, std::string* via);
