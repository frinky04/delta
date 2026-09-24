#pragma once
// Problems delta finds in a note's text, for the editor to mark.
#include <string>
#include <vector>

#include "editor.h"
#include "store.h"

// [[links]] that lead nowhere yet: to a space or note `ix` doesn't have.
// The target is underlined; code is skipped.
std::vector<Diagnostic> link_problems(const std::vector<std::string>& lines, const NoteIndex& ix);

// The [[links]] in a note in space `from` that lead to space/name (see
// NoteIndex::resolve): each one's line and target's byte range. Code is
// skipped, as above.
struct LinkHit { int line, col0, col1; };
std::vector<LinkHit> links_to(const std::vector<std::string>& lines, const std::string& from,
                              const std::string& space, const std::string& name, const NoteIndex& ix);
