#pragma once
// Highlighting for delta.ini when it's open in the editor. Uses the same
// per-byte style vectors as markdown, with its own flags. Every byte also
// carries S_CodeBlock so the whole file is set in the mono font; the flag
// bits below avoid the markdown bits the layout itself looks at
// (S_Marker, S_Code, S_TaskBox).
#include <cstdint>
#include <string>
#include <vector>

#include "markdown.h"

enum : uint16_t {
    I_Comment = 1 << 1,
    I_Section = 1 << 2,   // name inside [ ]
    I_Key     = 1 << 4,
    I_Eq      = 1 << 5,
    I_Value   = 1 << 6,
    I_Color   = 1 << 7,   // value is #rrggbb: gets a swatch
    I_Bracket = 1 << 9,
};

void ini_analyze(const std::string& s, LineInfo& info, std::vector<uint16_t>& style);

// "[name]" lines, for the @ palette.
bool ini_section(const std::string& s, std::string& name);
