#pragma once
// Chords like "ctrl+shift+p" <-> ImGui keys.
#include <string>
#include <vector>

#include "imgui.h"

struct Chord {
    ImGuiKey key = ImGuiKey_None;
    int mods = 0;  // ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super
};

bool parse_chord(const std::string& s, Chord& out);
// Whitespace-separated alternatives. Invalid tokens are appended to `bad`.
std::vector<Chord> parse_chords(const std::string& s, std::vector<std::string>* bad = nullptr);
std::string chord_str(const Chord& c);
bool chord_pressed(const Chord& c, bool repeat);
