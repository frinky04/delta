#pragma once
// Finding the words in a note worth spellchecking: prose only. Code, link
// targets, urls, paths, emails, #tags, html tags, snake_case and camelCase
// names, ALL CAPS, anything with a digit and single letters are left alone.
#include <string>
#include <vector>

struct WordSpan {
    int begin, end;  // bytes in the line
};

// Which lines have nothing to check whatever they say: fenced code blocks
// (fences included) and the front matter at the top of a note. Every other
// line's words depend only on its own text.
std::vector<bool> spell_skipped(const std::vector<std::string>& lines);

// The words in one (prose) line worth checking, in order.
std::vector<WordSpan> spell_words(const std::string& line);

// A word as a dictionary spells it: curly apostrophes made straight.
std::string spell_form(const std::string& line, WordSpan w);
