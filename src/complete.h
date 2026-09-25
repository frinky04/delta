#pragma once
// What the editor completes to as you write: #tags from every note, and
// notes to [[link]] to. Best first, at most 50.
#include <string>
#include <vector>

#include "editor.h"
#include "store.h"
#include "tags.h"

// Completing as you type, a match has to begin where a word does: "#r" is
// #rust, not #draft (the palette is looser; it's asked, not offered). `hl`
// is where the word matched in `s`.
bool starts_word(const std::string& s, const std::vector<int>& hl);

// Every tag in every note: tags mean the same everywhere. The open note
// (`open`, empty for none) counts as `lines`, less the tag ending at `caret`,
// the one being written.
std::vector<CompletionItem> complete_tags(const TagIndex& ix, const fs::path& open, const std::vector<std::string>& lines,
                                          TextPos caret, const std::string& word);

// Notes to link to: `space`'s by name, others' as "space/name". `own` is the
// open note's name (not linked to itself), or "".
std::vector<CompletionItem> complete_links(const NoteIndex& ix, const std::string& space, const std::string& own,
                                           const std::string& word);
