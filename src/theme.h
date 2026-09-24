#pragma once
#include <string>
#include <vector>

#include "imgui.h"

struct Config;
struct ConfigIssue;

struct Theme {
    bool dark = true;
    ImU32 bg, surface, border, text, bright, accent, code, code_bg, selection, highlight;
    ImU32 faint;     // separators and guides: never text, it's too quiet to read
    ImU32 dim;       // the quietest text, still readable (4.5:1 on bg)
    ImU32 error;     // marks problems (never their text)
    ImU32 spelling;  // marks misspelt words, quieter than error
};

extern Theme T;

// A built-in colour scheme: the twelve colours [colors] can override, as
// 0xRRGGBB, in the order kColourSlots names them.
struct ThemeDef {
    std::string name;
    unsigned colours[12];
    bool dark() const;  // by its background
};
extern const char* const kColourSlots[12];  // background, surface, ...

// Delta's own two first, then the rest in the order the picker lists them.
const std::vector<ThemeDef>& all_themes();
// The theme called `name`, or null.
const ThemeDef* find_theme(const std::string& name);
// A theme's colour for a [colors] slot, as "#rrggbb" ("" if no such slot).
// An unknown theme is delta's dark.
std::string theme_colour(const std::string& theme, const std::string& slot);

// Builds T from the config's theme and [colors]; `theme`, if given, stands
// in for the config's (the theme picker's preview).
void build_theme(const Config& cfg, const std::string* theme = nullptr);
void apply_imgui_style();

ImU32 col_alpha(ImU32 c, float a);          // multiply alpha
ImU32 col_mix(ImU32 a, ImU32 b, float t);   // lerp a -> b
unsigned col_rgb(ImU32 c);                  // 0xRRGGBB
