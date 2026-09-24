#pragma once
// Which fonts the text is drawn in: the embedded Noto faces, or ones the
// config names ([editor] font and mono_font).
#include <string>

#include "platform.h"

// The faces one font setting picks. An empty file is the embedded font.
struct FontPick {
    FontFace regular;
    FontFace bold;  // semibold or heavier; no file: none, so bold is drawn doubled
    bool embedded() const { return regular.file.empty(); }
};

// Reads a font setting: empty or `builtin` (the embedded font's name), an
// installed family ("JetBrains Mono"), or the path of a font file. Returns
// what's wrong ("" if nothing), and then `out` is the embedded font.
std::string pick_font(const std::string& value, const std::string& builtin, FontPick& out);
