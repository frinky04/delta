#pragma once
// delta.ini: the single place all settings live. Unknown keys are ignored,
// bad values fall back to defaults and are reported on their lines.
#include <map>
#include <string>
#include <utility>
#include <variant>
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
    std::string font = "Noto Sans";            // installed family or font file; the default is embedded
    std::string mono_font = "Noto Sans Mono";  // the same, for code
    float font_size = 14.0f;     // px, as in CSS (the em size)
    float line_height = 1.5f;    // multiple of font_size, as in CSS
    float code_size = 0.9f;      // multiple of font_size: code in notes
    float small_size = 0.8f;     // multiple of font_size: line numbers, image labels
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
    float ui_size = 0.9f;        // multiple of font_size: palette, status line, inline notes
    std::string palette_selection = "marker";  // marker (a > in the margin) or invert (a solid bar)
    float animation_speed = 1.0f;

    // [spell]
    bool spell = true;
    std::string spell_languages = "en_US";             // comma-separated dictionary names
    std::map<std::string, std::string> spell_spaces;   // space.<name>: that space's languages (lower-cased name)

    // [colors] name -> "#rrggbb" overrides on top of the theme
    std::map<std::string, std::string> colors;

    // [keys] command id -> chord list ("ctrl+z ctrl+shift+z"), empty = unbound
    std::map<std::string, std::string> keys;

    // Where each "section.key" was set (0-based line, byte columns of the key
    // and the value), so later checks (key chords, folders) can point at it.
    struct Where { int line = -1, key0 = 0, key1 = 0, val0 = 0, val1 = 0; };
    std::map<std::string, Where> where;
};

// A problem in the file. line is 0-based, or -1 when it isn't tied to a
// line; col0..col1 is the part of the line it's about (-1: the whole line).
struct ConfigIssue {
    int line = -1;
    std::string msg;
    int col0 = -1, col1 = -1;
};
// A problem with a setting's key or value, placed where it was set.
ConfigIssue issue_at(const Config& c, const std::string& section_key, bool value, std::string msg);

// Every setting delta knows: the one table the parser, the problems, the
// inline hints and "add setting" all work from. ([keys] comes from the
// command list instead; see BindingDoc.)
struct Setting {
    enum class Kind { Bool, Float, Int, Choice, Colour, Text, Path };
    const char* section;
    const char* key;
    Kind kind;
    const char* def;        // default, as written in the file ("" for colours)
    float lo, hi;           // Float and Int: allowed range
    const char* choices;    // Choice: "bar|thick|block"
    const char* desc;       // one short line, lowercase
    // The Config member it sets; none for colours, which go to Config::colors.
    using Field = std::variant<std::monostate, bool Config::*, int Config::*, float Config::*, std::string Config::*>;
    Field field;
    // The default under a theme: a colour's is the theme's own.
    std::string default_for(const std::string& theme) const;
    // Stores a checked value, as written in the file, in the field.
    void apply(Config& c, const std::string& value) const;
    // The field's value, as it would be written in the file.
    std::string read(const Config& c) const;
};
const std::vector<Setting>& all_settings();
const Setting* find_setting(const std::string& section, const std::string& key);
extern const char* const kSections[6];  // general, editor, appearance, spell, colors, keys

// A list of names as written in a setting ("en_US, en_GB" or "en_US en_GB").
std::vector<std::string> split_list(const std::string& v);

// The closest known name to a mistyped one, or "" if nothing is close.
std::string suggest(const std::string& typo, const std::vector<std::string>& names);
// How many edits suggest() allows for a typo, and the edits between two
// names, or limit + 1 once there are sure to be more than `limit`.
int suggest_limit(const std::string& typo);
int edit_distance(const std::string& a, const std::string& b, int limit);

// Returns false only if the file couldn't be read. Value problems land in
// `issues` but still produce a usable config.
bool load_config(const fs::path& path, Config& cfg, std::vector<ConfigIssue>& issues);
// The same, from text (the config as it's being edited).
void parse_config(const std::string& text, Config& cfg, std::vector<ConfigIssue>& issues);

// A command's id, title and default chords, for [keys].
struct BindingDoc { std::string id, title, keys; };

// The active `key = value` lines of a config being edited, with the section
// each sits in (lowercased names, value as written).
struct ConfigEntry { int line; std::string section, key, value; };
std::vector<ConfigEntry> scan_config(const std::vector<std::string>& lines);

// What delta shows beside each setting line: what it does and its default.
// Keyed by 0-based line.
std::map<int, std::string> config_hints(const std::vector<std::string>& lines, const std::string& theme,
                                        const std::vector<BindingDoc>& bindings);

// Where a new setting for `section` goes: just after the section's last
// setting (or its header). `header` is set when the section doesn't exist
// yet and "[section]" must be written first (at the returned line, the end).
int config_insert_line(const std::vector<std::string>& lines, const std::string& section, bool& header);

// An edit to a config's lines: bytes col0..col1 of `line` become `text`
// (which may run over several lines).
struct ConfigEdit {
    int line = 0, col0 = 0, col1 = 0;
    std::string text;
};
// The edit that sets section.key to `value`: the value replaced where it's
// set (the last time, which is the one that counts), or else the setting
// added where config_insert_line puts it, in which case `text` ends with
// the value.
ConfigEdit config_set(const std::vector<std::string>& lines, const std::string& section, const std::string& key,
                      const std::string& value);
void apply_edit(std::vector<std::string>& lines, const ConfigEdit& e);

// The file written on first launch: just the sections. Defaults and
// descriptions are shown beside each setting by delta itself, and "add a
// setting" inserts them, so nothing needs to live in the file.
std::string default_config_text();

// --sandbox: a config in `dir` whose notes folder is `dir/notes`, so a run
// starts as a first launch and never touches the real config or notes. A
// sandbox that's already there is used as it was left. Returns the config's
// path, or empty (with `err`) if it can't be made.
fs::path make_sandbox(const fs::path& dir, std::string* err);

// Tiny key=value store for app state (last note, window size...). Kept apart
// from delta.ini so the user's config never gets rewritten under them.
std::map<std::string, std::string> load_kv(const fs::path& path);
void save_kv(const fs::path& path, const std::map<std::string, std::string>& kv);
