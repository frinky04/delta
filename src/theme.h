#pragma once
#include <string>
#include <vector>

#include "imgui.h"

struct Config;
struct ConfigIssue;

struct Theme {
    bool dark = true;
    ImU32 bg, surface, border, faint, dim, text, bright, accent, code, code_bg, selection, highlight;
};

extern Theme T;

void build_theme(const Config& cfg, std::vector<ConfigIssue>& issues);
void apply_imgui_style();

ImU32 col_alpha(ImU32 c, float a);          // multiply alpha
ImU32 col_mix(ImU32 a, ImU32 b, float t);   // lerp a -> b
unsigned col_rgb(ImU32 c);                  // 0xRRGGBB
