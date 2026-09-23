#pragma once
// delta.ini: the single place all settings live. Unknown keys are ignored,
// bad values fall back to defaults and are reported in the status bar.
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "platform.h"

struct Config {
    // [general]
    std::string notes_dir = "~/Documents/delta";
    std::string default_space = "default";
    std::string daily_space = "daily";
    std::string daily_format = "%Y-%m-%d";
    bool new_note_heading = true;

    // [editor]
    float font_size = 14.0f;     // px, as in CSS (the em size)
    float line_height = 1.5f;    // multiple of font_size, as in CSS
    int column_width = 72;       // measured in "ch" (width of the digit 0)
    bool line_numbers = false;
    int tab_size = 4;
    float autosave_delay = 0.6f; // seconds of idle before writing to disk
    bool cursor_blink = true;
    std::string caret_style = "thick";  // bar, thick or block
    bool caret_trail = false;           // the editor caret smears as it moves
    bool fade_syntax = true;            // markdown symbols fade away from the caret
    float blank_line_height = 0.6f;     // empty lines, as a fraction of a line
    bool scroll_past_end = true;

    // [appearance]
    std::string theme = "dark";
    bool animations = true;
    std::string palette_selection = "marker";  // marker (a > in the margin) or invert (a solid bar)
    float animation_speed = 1.0f;

    // [colors] name -> "#rrggbb" overrides on top of the theme
    std::map<std::string, std::string> colors;

    // [keys] command id -> chord list ("ctrl+z ctrl+shift+z"), empty = unbound
    std::map<std::string, std::string> keys;

    // "section.key" -> 0-based line it was set on, so later checks (colours,
    // key chords) can point at the right line.
    std::map<std::string, int> key_lines;
    int line_of(const std::string& section_key) const {
        auto it = key_lines.find(section_key);
        return it == key_lines.end() ? -1 : it->second;
    }
};

// A problem in the file. line is 0-based, or -1 when it isn't tied to a line.
struct ConfigIssue {
    int line = -1;
    std::string msg;
};

// Returns false only if the file couldn't be read. Value problems land in
// `issues` but still produce a usable config.
bool load_config(const fs::path& path, Config& cfg, std::vector<ConfigIssue>& issues);

// The file written on first launch. Every setting is present but commented
// out, so the file documents all options while built-in defaults can still
// change; only lines the user uncomments override them. `bindings` is every
// command id with its title and default chords.
struct BindingDoc { std::string id, title, keys; };
std::string default_config_text(const std::vector<BindingDoc>& bindings);

// Tiny key=value store for app state (last note, window size...). Kept apart
// from delta.ini so the user's config never gets rewritten under them.
std::map<std::string, std::string> load_kv(const fs::path& path);
void save_kv(const fs::path& path, const std::map<std::string, std::string>& kv);
