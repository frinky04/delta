#include "fonts.h"

#include <fstream>
#include <vector>

#include "config.h"
#include "util.h"

// A path rather than a family name: it has a folder in it, or a font file's
// extension.
static bool is_file(const std::string& v) {
    if (v.find_first_of("/\\~") != std::string::npos) return true;
    std::string ext = lower(path_u8(u8path(v).extension()));
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

// A TrueType or OpenType font, or a collection of them: what ImGui can draw.
// (Not WOFF, or the old bitmap .fon.)
static bool is_font_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    char tag[4] = {};
    if (!in.read(tag, 4)) return false;
    std::string t(tag, 4);
    return t == std::string("\0\1\0\0", 4) || t == "OTTO" || t == "true" || t == "ttcf";
}

std::string pick_font(const std::string& value, const std::string& builtin, FontPick& out) {
    out = {};
    if (value.empty() || iequals(value, builtin)) return {};
    if (is_file(value)) {
        fs::path p = expand_user(value);
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return "no font file here, using " + builtin;
        if (!is_font_file(p)) return "not a font delta can read (.ttf, .otf or .ttc), using " + builtin;
        out.regular.file = p;  // one file is one weight: bold is drawn doubled
        return {};
    }

    FontFace regular;
    if (!find_system_font(value, 400, regular)) {
        std::vector<std::string> names = system_font_families();
        if (names.empty()) return "can't look up installed fonts here; give the font file's path. Using " + builtin;
        std::vector<std::string> lowered;
        for (auto& n : names) lowered.push_back(lower(n));
        std::string near = suggest(lower(value), lowered);
        // Installed, but not as a file here (a cloud font not yet downloaded).
        if (near == lower(value)) return "this font isn't on this computer as a file, using " + builtin;
        for (auto& n : names)
            if (!near.empty() && lower(n) == near) return "no installed font with this name; did you mean " + n + "?";
        return "no installed font with this name, using " + builtin;
    }
    if (!is_font_file(regular.file)) return "this font's file isn't one delta can read, using " + builtin;
    out.regular = regular;
    // A semibold that's the regular face again isn't one. That's a family
    // without heavier weights, or a variable font, whose other weights
    // ImGui can't draw.
    FontFace bold;
    if (find_system_font(value, 600, bold) && bold.weight >= 550 &&
        !(bold.file == regular.file && bold.index == regular.index) && is_font_file(bold.file))
        out.bold = bold;
    return {};
}
