#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "util.h"

// Each parser returns an empty string on success or what's wrong with the value.
static std::string parse_bool(const std::string& v, bool& out) {
    std::string l = lower(v);
    if (l == "true" || l == "yes" || l == "on" || l == "1") { out = true; return {}; }
    if (l == "false" || l == "no" || l == "off" || l == "0") { out = false; return {}; }
    return "expected true or false";
}

static std::string fmt_num(float f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", f);
    return buf;
}

static std::string parse_float(const std::string& v, float& out, float lo, float hi) {
    char* end = nullptr;
    float f = std::strtof(v.c_str(), &end);
    if (v.empty() || end == v.c_str() || *end) return "expected a number";
    out = std::clamp(f, lo, hi);
    if (out != f) return "out of range, using " + fmt_num(out) + " (" + fmt_num(lo) + " to " + fmt_num(hi) + ")";
    return {};
}

static std::string parse_int(const std::string& v, int& out, int lo, int hi) {
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (v.empty() || end == v.c_str() || *end) return "expected a whole number";
    out = std::clamp((int)n, lo, hi);
    if (out != n) return "out of range, using " + std::to_string(out) + " (" + std::to_string(lo) + " to " + std::to_string(hi) + ")";
    return {};
}

// Strips " ; comment" tails. A '#' is left alone so colours like #e0851e work.
static std::string strip_inline_comment(const std::string& v) {
    for (size_t i = 1; i < v.size(); i++)
        if (v[i] == ';' && (v[i - 1] == ' ' || v[i - 1] == '\t')) return trim(v.substr(0, i));
    return trim(v);
}

bool load_config(const fs::path& path, Config& cfg, std::vector<ConfigIssue>& issues) {
    std::string text;
    if (!read_file(path, text)) return false;

    Config c;  // start from defaults so removed keys revert
    std::string section;
    std::istringstream in(text);
    std::string raw;
    int lineno = -1;  // 0-based, to match editor lines
    while (std::getline(in, raw)) {
        lineno++;
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        auto issue = [&](std::string msg) { issues.push_back({lineno, std::move(msg)}); };
        if (line.front() == '[') {
            if (line.back() != ']') { issue("missing ] after the section name"); continue; }
            section = lower(trim(line.substr(1, line.size() - 2)));
            static const char* known[] = {"general", "editor", "appearance", "colors", "keys"};
            if (std::find_if(std::begin(known), std::end(known), [&](const char* k) { return section == k; }) == std::end(known))
                issue("unknown section; use general, editor, appearance, colors or keys");
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { issue("expected key = value"); continue; }
        std::string key = lower(trim(line.substr(0, eq)));
        std::string val = strip_inline_comment(line.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (section.empty()) { issue("setting outside any [section]"); continue; }
        c.key_lines[section + "." + key] = lineno;

        std::string err;
        bool known = true;
        if (section == "general") {
            if (key == "notes_dir") {
                // An empty or relative path would land wherever delta was
                // started from, so both are refused.
                if (val.empty()) err = "can't be empty, using " + c.notes_dir;
                else if (!expand_user(val).is_absolute()) err = "use a full path or one starting with ~, using " + c.notes_dir;
                else c.notes_dir = val;
            }
            else if (key == "default_space") c.default_space = val;
            else if (key == "daily_space") c.daily_space = val;
            else if (key == "daily_format") c.daily_format = val;
            else if (key == "new_note_heading") err = parse_bool(val, c.new_note_heading);
            else known = false;
        } else if (section == "editor") {
            if (key == "font_size") err = parse_float(val, c.font_size, 8, 48);
            else if (key == "line_height") err = parse_float(val, c.line_height, 1.0f, 3.0f);
            else if (key == "column_width") err = parse_int(val, c.column_width, 20, 400);
            else if (key == "line_numbers") err = parse_bool(val, c.line_numbers);
            else if (key == "tab_size") err = parse_int(val, c.tab_size, 1, 8);
            else if (key == "autosave_delay") err = parse_float(val, c.autosave_delay, 0.1f, 60.0f);
            else if (key == "cursor_blink") err = parse_bool(val, c.cursor_blink);
            else if (key == "fade_syntax") err = parse_bool(val, c.fade_syntax);
            else if (key == "caret_trail") err = parse_bool(val, c.caret_trail);
            else if (key == "blank_line_height") err = parse_float(val, c.blank_line_height, 0.2f, 1.0f);
            else if (key == "caret_style") {
                std::string v = lower(val);
                if (v == "bar" || v == "thick" || v == "block") c.caret_style = v;
                else err = "use bar, thick or block";
            }
            else if (key == "scroll_past_end") err = parse_bool(val, c.scroll_past_end);
            else known = false;
        } else if (section == "appearance") {
            if (key == "theme") c.theme = lower(val);
            else if (key == "animations") err = parse_bool(val, c.animations);
            else if (key == "palette_selection") {
                std::string v = lower(val);
                if (v == "marker" || v == "invert") c.palette_selection = v;
                else err = "use marker or invert";
            }
            else if (key == "animation_speed") err = parse_float(val, c.animation_speed, 0.1f, 10.0f);
            else known = false;
        } else if (section == "colors") {
            c.colors[key] = val;  // checked by the theme
        } else if (section == "keys") {
            c.keys[key] = val;    // checked against the command list
        }
        if (!known) issue("unknown setting");
        else if (!err.empty()) issue(err);
    }
    cfg = std::move(c);
    return true;
}

std::string default_config_text(const std::vector<BindingDoc>& bindings) {
    std::ostringstream o;
    o << "; delta configuration\n"
         "; Every setting below shows its default and is commented out. Remove the\n"
         "; leading ; to change one. Saved changes apply immediately.\n"
         "\n"
         "[general]\n"
         "; Where spaces (folders) and notes (.md files) live. ~ is your home folder.\n"
         ";notes_dir = ~/Documents/delta\n"
         "; Space used when nothing else is picked.\n"
         ";default_space = default\n"
         "; Daily notes go here, named with daily_format (strftime codes).\n"
         ";daily_space = daily\n"
         ";daily_format = %Y-%m-%d\n"
         "; Start new notes with a # heading of their name.\n"
         ";new_note_heading = true\n"
         "\n"
         "[editor]\n"
         "; Text size in px and line height as a multiple of it, as in CSS.\n"
         ";font_size = 14\n"
         ";line_height = 1.5\n"
         "; Width of the text column, in characters.\n"
         ";column_width = 72\n"
         ";line_numbers = false\n"
         "; Markdown symbols (**, #, [](url)) fade except on the line you're editing.\n"
         ";fade_syntax = true\n"
         "; Height of an empty line, as a fraction of a full line. 1 = no difference.\n"
         ";blank_line_height = 0.6\n"
         ";tab_size = 4\n"
         "; Seconds after your last keystroke before the note is written to disk.\n"
         ";autosave_delay = 0.6\n"
         ";cursor_blink = true\n"
         "; bar (thin line), thick (a fat bar) or block (covers the next character)\n"
         ";caret_style = thick\n"
         "; The caret smears as it moves, like Neovide's.\n"
         ";caret_trail = false\n"
         ";scroll_past_end = true\n"
         "\n"
         "[appearance]\n"
         "; dark or light\n"
         ";theme = dark\n"
         ";animations = true\n"
         "; Palette selection: marker (a > in the margin) or invert (a solid bar)\n"
         ";palette_selection = marker\n"
         "; 2 = twice as fast, 0.5 = half speed\n"
         ";animation_speed = 1.0\n"
         "\n"
         "[colors]\n"
         "; Override any theme colour with #rrggbb (dark theme values shown).\n"
         ";accent = #e6e6e6\n"
         ";background = #0c0c0c\n"
         ";surface = #151515\n"
         ";border = #242424\n"
         ";text = #bdbdbd\n"
         ";bright = #f0f0f0\n"
         ";dim = #6b6b6b\n"
         ";faint = #383838\n"
         ";code = #9e9e9e\n"
         ";code_background = #161616\n"
         "\n"
         "[keys]\n"
         "; command = chord [chord ...]   e.g.  note.daily = alt+d ctrl+shift+d\n"
         "; Modifiers: ctrl shift alt super. Set to nothing to unbind.\n"
         "; Every command in the / palette can be bound here.\n";
    size_t w = 0;
    for (auto& b : bindings) w = std::max(w, b.id.size());
    for (auto& b : bindings) {
        o << "; " << b.title << "\n";
        o << ";" << b.id << std::string(w - b.id.size(), ' ') << " = " << b.keys << "\n";
    }
    return o.str();
}

std::map<std::string, std::string> load_kv(const fs::path& path) {
    std::map<std::string, std::string> kv;
    std::string text;
    if (!read_file(path, text)) return kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return kv;
}

void save_kv(const fs::path& path, const std::map<std::string, std::string>& kv) {
    std::string out;
    for (auto& [k, v] : kv) out += k + " = " + v + "\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    atomic_write(path, out, nullptr);
}
