#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <tuple>
#include <type_traits>

#include "ini_syntax.h"
#include "theme.h"
#include "util.h"

using K = Setting::Kind;

const char* const kSections[6] = {"general", "editor", "appearance", "spell", "colors", "keys"};

std::vector<std::string> split_list(const std::string& v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v + ",") {
        if (c == ',' || c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

const std::vector<Setting>& all_settings() {
    static const std::string themes = [] {
        std::string s;
        for (auto& t : all_themes()) s += (s.empty() ? "" : "|") + t.name;
        return s;
    }();
    // Colours have no default of their own: it's the theme's (default_for).
    static const std::vector<Setting> table = {
        // section, key, kind, default, lo, hi, choices, description, field
        // (descriptions only where the name doesn't say it all)
        {"general", "notes_dir", K::Path, "~/Documents/delta", 0, 0, nullptr, "~ is home", &Config::notes_dir},
        {"general", "default_space", K::Text, "default", 0, 0, nullptr, "", &Config::default_space},
        {"general", "daily_space", K::Text, "daily", 0, 0, nullptr, "", &Config::daily_space},
        {"general", "daily_format", K::Text, "%Y-%m-%d", 0, 0, nullptr, "strftime codes", &Config::daily_format},
        {"general", "new_note_heading", K::Bool, "true", 0, 0, nullptr, "name as first heading", &Config::new_note_heading},

        {"editor", "font", K::Text, "Noto Sans", 0, 0, nullptr, "an installed font's name, or a font file", &Config::font},
        {"editor", "mono_font", K::Text, "Noto Sans Mono", 0, 0, nullptr, "for code; a name or a file", &Config::mono_font},
        {"editor", "font_size", K::Float, "14", 8, 48, nullptr, "px", &Config::font_size},
        {"editor", "line_height", K::Float, "1.5", 1, 3, nullptr, "x font size", &Config::line_height},
        {"editor", "code_size", K::Float, "0.9", 0.5f, 1.5f, nullptr, "x font size; code", &Config::code_size},
        {"editor", "small_size", K::Float, "0.8", 0.5f, 1.5f, nullptr, "x font size; line numbers, image labels", &Config::small_size},
        {"editor", "column_width", K::Int, "72", 20, 400, nullptr, "characters", &Config::column_width},
        {"editor", "line_numbers", K::Bool, "false", 0, 0, nullptr, "", &Config::line_numbers},
        {"editor", "tab_size", K::Int, "4", 1, 8, nullptr, "spaces", &Config::tab_size},
        {"editor", "autosave_delay", K::Float, "0.6", 0.1f, 60, nullptr, "seconds idle", &Config::autosave_delay},
        {"editor", "cursor_blink", K::Bool, "true", 0, 0, nullptr, "", &Config::cursor_blink},
        {"editor", "caret_style", K::Choice, "thick", 0, 0, "bar|thick|block", "bar, thick or block", &Config::caret_style},
        {"editor", "caret_trail", K::Bool, "false", 0, 0, nullptr, "smears as it moves", &Config::caret_trail},
        {"editor", "fade_syntax", K::Bool, "true", 0, 0, nullptr, "dim markdown away from the caret", &Config::fade_syntax},
        {"editor", "blank_line_height", K::Float, "0.6", 0.2f, 1, nullptr, "x line height", &Config::blank_line_height},
        {"editor", "scroll_past_end", K::Bool, "true", 0, 0, nullptr, "", &Config::scroll_past_end},

        {"appearance", "theme", K::Choice, "dark", 0, 0, themes.c_str(), "run \"choose theme\" to see them", &Config::theme},
        {"appearance", "animations", K::Bool, "true", 0, 0, nullptr, "", &Config::animations},
        {"appearance", "ui_size", K::Float, "0.9", 0.5f, 1.5f, nullptr, "x font size; palette, status line, notes", &Config::ui_size},
        {"appearance", "animation_speed", K::Float, "1", 0.1f, 10, nullptr, "2 is twice as fast", &Config::animation_speed},
        {"appearance", "palette_selection", K::Choice, "marker", 0, 0, "marker|invert", "marker or invert", &Config::palette_selection},

        {"spell", "enabled", K::Bool, "true", 0, 0, nullptr, "underline misspelt words", &Config::spell},
        {"spell", "languages", K::Text, "en_US", 0, 0, nullptr, "dictionaries, comma-separated", &Config::spell_languages},

        {"colors", "accent", K::Colour, "", 0, 0, nullptr, "caret, selection"},
        {"colors", "background", K::Colour, "", 0, 0, nullptr, ""},
        {"colors", "surface", K::Colour, "", 0, 0, nullptr, "raised panels"},
        {"colors", "border", K::Colour, "", 0, 0, nullptr, ""},
        {"colors", "faint", K::Colour, "", 0, 0, nullptr, "separators and guides"},
        {"colors", "dim", K::Colour, "", 0, 0, nullptr, "symbols, secondary text"},
        {"colors", "text", K::Colour, "", 0, 0, nullptr, ""},
        {"colors", "bright", K::Colour, "", 0, 0, nullptr, "headings, bold"},
        {"colors", "code", K::Colour, "", 0, 0, nullptr, ""},
        {"colors", "code_background", K::Colour, "", 0, 0, nullptr, ""},
        {"colors", "error", K::Colour, "", 0, 0, nullptr, "problem marks"},
        {"colors", "spelling", K::Colour, "", 0, 0, nullptr, "misspelt word marks"},
    };
    return table;
}

std::string Setting::default_for(const std::string& theme) const {
    return kind == Kind::Colour ? theme_colour(theme, key) : def;
}

const Setting* find_setting(const std::string& section, const std::string& key) {
    for (auto& s : all_settings())
        if (section == s.section && key == s.key) return &s;
    return nullptr;
}

int edit_distance(const std::string& a, const std::string& b, int limit) {
    if (std::abs((int)a.size() - (int)b.size()) > limit) return limit + 1;
    // Two rows, kept between calls: suggest() runs this against every name.
    thread_local std::vector<int> prev, cur;
    prev.resize(b.size() + 1);
    cur.resize(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = (int)i;
        int row_min = cur[0];
        for (size_t j = 1; j <= b.size(); j++) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > limit) return limit + 1;  // no later row gets below this
        std::swap(prev, cur);
    }
    return std::min(prev[b.size()], limit + 1);
}

// Close enough to be a typo: a couple of edits, or a third of the name.
int suggest_limit(const std::string& typo) { return std::max(2, (int)typo.size() / 3); }

std::string suggest(const std::string& typo, const std::vector<std::string>& names) {
    const int limit = suggest_limit(typo);
    std::string best;
    int best_d = limit + 1;
    for (auto& n : names) {
        int d = edit_distance(typo, n, best_d - 1);  // only a nearer one counts
        if (d < best_d) { best_d = d; best = n; }
    }
    return best_d <= limit ? best : std::string();
}

static std::string fmt_num(float f) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", f);
    return buf;
}

static bool parse_bool_value(const std::string& v, bool& out) {
    std::string l = lower(v);
    if (l == "true" || l == "yes" || l == "on" || l == "1") { out = true; return true; }
    if (l == "false" || l == "no" || l == "off" || l == "0") { out = false; return true; }
    return false;
}

void Setting::apply(Config& c, const std::string& value) const {
    std::visit([&](auto f) {
        using T = decltype(f);
        if constexpr (std::is_same_v<T, bool Config::*>) { bool b = false; parse_bool_value(value, b); c.*f = b; }
        else if constexpr (std::is_same_v<T, int Config::*>) c.*f = (int)(float)std::atof(value.c_str());
        else if constexpr (std::is_same_v<T, float Config::*>) c.*f = (float)std::atof(value.c_str());
        else if constexpr (std::is_same_v<T, std::string Config::*>) c.*f = value;
    }, field);
}

std::string Setting::read(const Config& c) const {
    return std::visit([&](auto f) -> std::string {
        using T = decltype(f);
        if constexpr (std::is_same_v<T, bool Config::*>) return c.*f ? "true" : "false";
        else if constexpr (std::is_same_v<T, int Config::*>) return std::to_string(c.*f);
        else if constexpr (std::is_same_v<T, float Config::*>) return fmt_num(c.*f);
        else if constexpr (std::is_same_v<T, std::string Config::*>) return c.*f;
        else return {};
    }, field);
}

// Does `value` say the same as `def` for this kind of setting (1 == 1.0,
// yes == true, BLOCK == block)?
static bool same_value(const Setting& s, const std::string& value, const std::string& def) {
    switch (s.kind) {
        case K::Bool: {
            bool a, b;
            return parse_bool_value(value, a) && parse_bool_value(def, b) && a == b;
        }
        case K::Float:
        case K::Int: {
            char* e1 = nullptr;
            char* e2 = nullptr;
            double a = std::strtod(value.c_str(), &e1), b = std::strtod(def.c_str(), &e2);
            return e1 != value.c_str() && !*e1 && std::fabs(a - b) < 1e-6;
        }
        default:
            return lower(value) == lower(def);
    }
}

// Checks a value against its setting. Returns what's wrong ("" if fine) and
// the value to use (clamped into range, or the default when unusable).
static std::string check(const Setting& s, const std::string& v, std::string& use) {
    use = v;
    switch (s.kind) {
        case K::Bool: {
            bool b;
            if (!parse_bool_value(v, b)) { use = s.def; return std::string("expected true or false, using ") + s.def; }
            use = b ? "true" : "false";
            return {};
        }
        case K::Float:
        case K::Int: {
            char* end = nullptr;
            double n = s.kind == K::Int ? (double)std::strtol(v.c_str(), &end, 10) : std::strtod(v.c_str(), &end);
            std::string range = fmt_num(s.lo) + " to " + fmt_num(s.hi);
            if (v.empty() || end == v.c_str() || *end || std::isnan(n)) {  // clamp passes NaN through
                use = s.def;
                return std::string(s.kind == K::Int ? "expected a whole number, " : "expected a number, ") + range + ", using " + s.def;
            }
            double c = std::clamp(n, (double)s.lo, (double)s.hi);
            use = fmt_num((float)c);
            if (c != n) return "out of range (" + range + "), using " + use;
            return {};
        }
        case K::Choice: {
            std::string l = lower(v), opts = s.choices;
            std::vector<std::string> list;
            for (size_t a = 0, b; a <= opts.size(); a = b + 1) {
                b = opts.find('|', a);
                if (b == std::string::npos) b = opts.size();
                list.push_back(opts.substr(a, b - a));
            }
            if (std::find(list.begin(), list.end(), l) != list.end()) { use = l; return {}; }
            use = s.def;
            if (list.size() > 4) {  // too many to list: the nearest one
                std::string near = suggest(l, list);
                return std::string("no ") + s.key + " with this name; " + (near.empty() ? "" : "did you mean " + near + "? ") +
                       "using " + s.def;
            }
            std::string msg = "use ";
            for (size_t i = 0; i < list.size(); i++) msg += (i ? (i + 1 == list.size() ? " or " : ", ") : "") + list[i];
            return msg + ", using " + s.def;
        }
        case K::Colour:
            if (!is_hex_colour(v)) { use.clear(); return "expected a colour like #e0851e, using the default"; }
            return {};
        case K::Path:
            // An empty or relative path would land wherever delta was started
            // from, so both are refused.
            if (v.empty()) { use = s.def; return std::string("can't be empty, using ") + s.def; }
            if (!expand_user(v).is_absolute()) { use = s.def; return std::string("use a full path or one starting with ~, using ") + s.def; }
            return {};
        case K::Text:
            return {};
    }
    return {};
}

// Where the value after the = at `eq` ends: at a " ; comment", or the line's end.
static size_t value_end(const std::string& s, size_t eq) {
    size_t c = ini_comment(s, eq + 1);
    return c == std::string::npos ? s.size() : c;
}

// A value written in quotes, without them.
static std::string unquote(std::string v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

// The name in a "[name]" line (already trimmed), lower-cased.
static std::string section_of(const std::string& line) { return lower(trim(line.substr(1, line.size() - 2))); }

ConfigIssue issue_at(const Config& c, const std::string& section_key, bool value, std::string msg) {
    auto it = c.where.find(section_key);
    if (it == c.where.end()) return {-1, std::move(msg)};
    const Config::Where& w = it->second;
    // An empty value has nothing to underline; point at the key instead.
    if (value && w.val1 > w.val0) return {w.line, std::move(msg), w.val0, w.val1};
    return {w.line, std::move(msg), w.key0, w.key1};
}

// [a, b) of s with blanks trimmed off both ends.
static std::pair<int, int> trim_span(const std::string& s, size_t a, size_t b) {
    auto blank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; };
    while (a < b && blank(s[a])) a++;
    while (b > a && blank(s[b - 1])) b--;
    return {(int)a, (int)b};
}

bool load_config(const fs::path& path, Config& cfg, std::vector<ConfigIssue>& issues) {
    std::string text;
    if (!read_file(path, text)) return false;
    parse_config(text, cfg, issues);
    return true;
}

void parse_config(const std::string& text, Config& cfg, std::vector<ConfigIssue>& issues) {
    Config c;  // start from defaults so removed keys revert
    std::map<std::string, std::string> vals;  // "section.key" -> checked value
    std::vector<std::string> sections(std::begin(kSections), std::end(kSections));
    std::string section;
    std::istringstream in(text);
    std::string raw;
    int lineno = -1;  // 0-based, to match editor lines
    while (std::getline(in, raw)) {
        lineno++;
        std::string line = trim(raw);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        auto [l0, l1] = trim_span(raw, 0, raw.size());
        auto issue = [&](std::string msg, std::pair<int, int> span) { issues.push_back({lineno, std::move(msg), span.first, span.second}); };
        if (line.front() == '[') {
            if (line.back() != ']') { issue("missing ] after the section name", {l0, l1}); continue; }
            section = section_of(line);
            if (std::find(sections.begin(), sections.end(), section) == sections.end()) {
                std::string near = suggest(section, sections);
                issue(near.empty() ? "unknown section; use general, editor, appearance, spell, colors or keys"
                                   : "unknown section; did you mean [" + near + "]?",
                      trim_span(raw, l0 + 1, l1 - 1));
            }
            continue;
        }
        size_t eq = raw.find('=');
        if (eq == std::string::npos) { issue("expected key = value", {l0, l1}); continue; }
        size_t vend = value_end(raw, eq);
        Config::Where w;
        w.line = lineno;
        std::tie(w.key0, w.key1) = trim_span(raw, 0, eq);
        std::tie(w.val0, w.val1) = trim_span(raw, eq + 1, vend);
        std::pair<int, int> key_span{w.key0, w.key1}, val_span{w.val0, w.val1};
        if (w.val1 <= w.val0) val_span = key_span;

        std::string key = lower(trim(raw.substr(0, eq)));
        std::string val = unquote(trim(raw.substr(eq + 1, vend - eq - 1)));
        if (section.empty()) { issue("setting outside any [section]", key_span); continue; }
        c.where[section + "." + key] = w;
        if (section == "keys") { c.keys[key] = val; continue; }  // checked against the commands
        if (section == "spell" && key.rfind("space.", 0) == 0) {  // checked against the spaces and dictionaries
            std::string space = trim(key.substr(6));
            if (space.empty()) issue("name a space after space.", key_span);
            else c.spell_spaces[space] = val;
            continue;
        }

        const Setting* s = find_setting(section, key);
        if (!s) {
            // A typo, or the right name in the wrong section?
            for (auto& other : all_settings())
                if (key == other.key) { s = &other; break; }
            if (s) { issue(std::string("belongs in [") + s->section + "]", key_span); continue; }
            std::vector<std::string> names;
            for (auto& o : all_settings())
                if (section == o.section) names.push_back(o.key);
            std::string near = suggest(key, names);
            issue(near.empty() ? "unknown setting" : "unknown setting; did you mean " + near + "?", key_span);
            continue;
        }
        std::string use;
        std::string err = check(*s, val, use);
        if (!err.empty()) issue(err, val_span);
        if (!use.empty()) vals[section + "." + key] = use;
    }

    // Typed values, falling back to each setting's default.
    for (auto& s : all_settings()) {
        auto it = vals.find(std::string(s.section) + "." + s.key);
        if (s.kind == K::Colour) {
            if (it != vals.end()) c.colors[s.key] = it->second;
        } else {
            s.apply(c, it != vals.end() ? it->second : std::string(s.def));
        }
    }

    cfg = std::move(c);
}

std::vector<ConfigEntry> scan_config(const std::vector<std::string>& lines) {
    std::vector<ConfigEntry> out;
    std::string section;
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string line = trim(lines[i]);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[') {
            if (line.back() == ']') section = section_of(line);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        out.push_back({i, section, lower(trim(line.substr(0, eq))), trim(line.substr(eq + 1, value_end(line, eq) - eq - 1))});
    }
    return out;
}

// Chord lists compare by what they bind, not how they're spelled.
static std::string chord_key(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream in(lower(s));
    for (std::string w; in >> w;) words.push_back(w);
    std::sort(words.begin(), words.end());
    std::string out;
    for (auto& w : words) out += w + " ";
    return out;
}

// "px, default 14", or "px (default)" when the value already is it.
static std::string hint(const std::string& desc, bool is_default, const std::string& def) {
    std::string d = is_default ? "(default)" : def.empty() ? "unbound by default" : "default " + def;
    if (desc.empty()) return d;
    return desc + (is_default ? " " : ", ") + d;
}

std::map<int, std::string> config_hints(const std::vector<std::string>& lines, const std::string& theme,
                                        const std::vector<BindingDoc>& bindings) {
    std::map<int, std::string> hints;
    for (auto& e : scan_config(lines)) {
        if (e.section == "keys") {
            // Command ids name themselves; only the default chords are news.
            for (auto& b : bindings)
                if (b.id == e.key) hints[e.line] = hint("", chord_key(e.value) == chord_key(b.keys), b.keys);
            continue;
        }
        if (e.section == "spell" && e.key.rfind("space.", 0) == 0) {
            hints[e.line] = "dictionaries for this space";
            continue;
        }
        const Setting* s = find_setting(e.section, e.key);
        if (!s) continue;  // the problem shows instead
        std::string def = s->default_for(theme);
        hints[e.line] = hint(s->desc, same_value(*s, unquote(e.value), def), def);
    }
    return hints;
}

int config_insert_line(const std::vector<std::string>& lines, const std::string& section, bool& header) {
    header = false;
    int at = -1;
    std::string cur;
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string line = trim(lines[i]);
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            cur = section_of(line);
            if (cur == section && at < 0) at = i + 1;
            continue;
        }
        // Settings (and commented-out ones) extend the section; blank lines
        // and comments between sections don't.
        if (cur == section && !line.empty() && line.find('=') != std::string::npos) at = i + 1;
    }
    if (at >= 0) return at;
    header = true;
    return (int)lines.size();
}

ConfigEdit config_set(const std::vector<std::string>& lines, const std::string& section, const std::string& key,
                      const std::string& value) {
    // A " ;" in it would start a comment: quoted, it's kept.
    std::string v = value;
    if (v.find(" ;") != std::string::npos || v.find("\t;") != std::string::npos) v = "\"" + v + "\"";

    ConfigEdit e;
    const ConfigEntry* set = nullptr;
    std::vector<ConfigEntry> entries = scan_config(lines);
    for (auto& en : entries)
        if (en.section == section && en.key == key) set = &en;
    if (set) {
        // The value as parse_config reads it: after the =, up to a " ; comment".
        const std::string& raw = lines[set->line];
        size_t eq = raw.find('='), vend = value_end(raw, eq);
        auto [a, b] = trim_span(raw, eq + 1, vend);
        if (a == b) {  // no value yet: whatever's after the = becomes " value"
            a = (int)eq + 1;
            b = (int)vend;
            v = " " + v + (vend < raw.size() ? " " : "");
        }
        e.line = set->line;
        e.col0 = a;
        e.col1 = b;
        e.text = v;
        return e;
    }

    // Written at the end of the line before, so the new text ends exactly
    // after the value.
    bool header = false;
    int at = config_insert_line(lines, section, header);
    e.text = "\n";
    if (header) {
        bool empty_file = lines.empty() || (lines.size() == 1 && lines[0].empty());
        e.text = empty_file ? "" : lines.back().empty() ? "\n" : "\n\n";
        e.text += "[" + section + "]\n";
    }
    e.text += key + " = " + v;
    e.line = std::max(0, at - 1);
    e.col0 = e.col1 = lines.empty() ? 0 : (int)lines[e.line].size();
    return e;
}

void apply_edit(std::vector<std::string>& lines, const ConfigEdit& e) {
    if (lines.empty()) lines.emplace_back();
    const std::string& s = lines[e.line];
    std::string joined = s.substr(0, e.col0) + e.text + s.substr(e.col1);
    std::vector<std::string> parts;
    for (size_t a = 0, b;; a = b + 1) {
        b = joined.find('\n', a);
        parts.push_back(joined.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
    }
    lines.erase(lines.begin() + e.line);
    lines.insert(lines.begin() + e.line, parts.begin(), parts.end());
}

std::string default_config_text() {
    return "; delta config: anything set here overrides the default\n"
           "; in delta (ctrl+,) each setting shows its default beside it; \"add setting\" (ctrl+p) adds one\n"
           "\n"
           "[general]\n"
           "\n"
           "[editor]\n"
           "\n"
           "[appearance]\n"
           "\n"
           "[spell]\n"
           "\n"
           "[colors]\n"
           "\n"
           "[keys]\n";
}

fs::path make_sandbox(const fs::path& dir, std::string* err) {
    std::error_code ec;
    fs::path notes = dir / "notes";
    fs::create_directories(notes, ec);
    if (ec) { if (err) *err = plain_msg(ec.message()); return {}; }
    fs::path cfg = dir / "delta.ini";
    if (!fs::exists(cfg, ec)) {
        std::string text = default_config_text();
        const std::string general = "[general]\n";
        text.insert(text.find(general) + general.size(), "notes_dir = " + path_u8(fs::absolute(notes, ec).make_preferred()) + "\n");
        if (!atomic_write(cfg, text, err)) return {};
    }
    return cfg;
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
