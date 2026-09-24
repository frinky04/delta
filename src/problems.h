#pragma once
// Problems delta finds in a note's text, for the editor to mark.
#include <string>
#include <vector>

#include "editor.h"
#include "store.h"

// [[links]] that lead nowhere yet: to a space or note `ix` doesn't have.
// The target is underlined; code is skipped.
std::vector<Diagnostic> link_problems(const std::vector<std::string>& lines, const NoteIndex& ix);
