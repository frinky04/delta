// Headless tests: drives the real Editor through ImGui frames with simulated
// input. No window or GPU; font textures are requested but never uploaded.
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
#include "images.h"
#include "platform.h"
#include "imgui.h"
#include "ini_syntax.h"
#include "smear.h"
#include "theme.h"

#include <algorithm>
#include "markdown.h"
#include "palette.h"
#include "keys.h"

extern const unsigned char g_font_sans[];
extern const std::size_t g_font_sans_size;
extern const unsigned char g_font_mono[];
extern const std::size_t g_font_mono_size;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) g_pass++; else { g_fail++; std::printf("FAIL %s:%d  %s\n   got: [%s]\n  want: [%s]\n", __FILE__, __LINE__, #a, to_s(_a).c_str(), to_s(_b).c_str()); } } while (0)

static std::string to_s(const std::string& s) { return s; }
static std::string to_s(const char* s) { return s; }
static std::string to_s(int v) { return std::to_string(v); }
static std::string to_s(size_t v) { return std::to_string(v); }
static std::string to_s(bool v) { return v ? "true" : "false"; }

static ImFont* g_sans;
static ImFont* g_mono;
static double g_time = 0;
static float g_dt_override = 0;  // simulate a long idle gap before a frame
static ImageSource* g_images = nullptr;  // images the editor can "see" (see test_images)

static void frame(Editor& ed, bool reader_keys = true) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = g_dt_override > 0 ? g_dt_override : 1.0f / 60;
    anim::tick(io.DeltaTime, false, 0.0f);  // as the main loop does (no display rate here)
    g_time += io.DeltaTime;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration);
    EditorParams p;
    p.sans = g_sans;
    p.mono = g_mono;
    p.size = 17;
    p.column = 600;
    p.min = ImVec2(0, 0);
    p.max = ImVec2(1000, 760);
    p.keyboard = reader_keys;
    p.images = g_images;
    ed.draw(p);
    ImGui::End();
    ImGui::Render();
}

static void key(Editor& ed, ImGuiKey k, int mods = 0) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (mods & ImGuiMod_Ctrl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & ImGuiMod_Shift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (mods & ImGuiMod_Alt) != 0);
    io.AddKeyEvent(k, true);
    frame(ed);
    io.AddKeyEvent(k, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
    frame(ed);
}

static void type(Editor& ed, const char* s) {
    ImGui::GetIO().AddInputCharactersUTF8(s);
    frame(ed);
}

static Editor fresh(const std::string& text) {
    Editor ed;
    ed.set_text(text);
    frame(ed);
    return ed;
}

static void test_markdown() {
    LineInfo info;
    std::vector<uint16_t> st;

    md_analyze("## Title", false, info, st);
    CHECK(info.kind == LineKind::Heading);
    CHECK_EQ(info.level, 2);
    CHECK(st[0] & S_Marker);
    CHECK(!(st[3] & S_Marker));

    md_analyze("a **bold** b", false, info, st);
    CHECK((st[2] & S_Marker) && (st[2] & S_Bold));
    CHECK((st[4] & S_Bold) && !(st[4] & S_Marker));
    CHECK(!(st[0] & S_Bold));

    md_analyze("x *it* y", false, info, st);
    CHECK(st[3] & S_Italic);

    md_analyze("snake_case_name here", false, info, st);
    CHECK(!(st[6] & S_Italic));  // intraword underscores are not emphasis

    md_analyze("`**not bold**`", false, info, st);
    CHECK(st[3] & S_Code);
    CHECK(!(st[3] & S_Bold));

    md_analyze("  - [x] done", false, info, st);
    CHECK(info.list && info.task && info.done);
    CHECK_EQ(info.marker_begin, 2);
    CHECK_EQ(info.content_start, 8);
    CHECK(st[9] & S_TaskDone);

    md_analyze("3. third", false, info, st);
    CHECK(info.ordered);
    CHECK_EQ(info.content_start, 3);

    md_analyze("---", false, info, st);
    CHECK(info.kind == LineKind::Rule);

    md_analyze("- a", false, info, st);
    CHECK(info.list);
    CHECK(info.kind != LineKind::Rule);

    std::vector<LinkSpan> links;
    md_analyze("see [[Other note|alias]] and [x](https://a.b)", false, info, st, &links);
    CHECK_EQ((int)links.size(), 2);
    if (links.size() == 2) {
        CHECK_EQ(links[0].target, std::string("Other note"));
        CHECK(links[0].wiki);
        CHECK_EQ(links[1].target, std::string("https://a.b"));
    }

    md_analyze("```js", false, info, st);
    CHECK(info.kind == LineKind::Fence);
    md_analyze("# not heading", true, info, st);
    CHECK(info.kind == LineKind::CodeBlock);

    std::string out;
    std::vector<uint16_t> ost;
    std::vector<int> map;
    md_analyze("## Hello **world**", false, info, st);
    md_reader("## Hello **world**", info, st, out, ost, map);
    CHECK_EQ(out, std::string("Hello world"));
    md_analyze("- item", false, info, st);
    md_reader("- item", info, st, out, ost, map);
    CHECK_EQ(out, std::string("\xE2\x80\xA2 item"));
}

static void test_typing() {
    Editor ed = fresh("");
    type(ed, "hello");
    CHECK_EQ(ed.text(), std::string("hello"));
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("hell"));
    key(ed, ImGuiKey_Backspace, ImGuiMod_Ctrl);
    CHECK_EQ(ed.text(), std::string(""));
    type(ed, "one two");
    key(ed, ImGuiKey_LeftArrow, ImGuiMod_Ctrl);
    type(ed, "X");
    CHECK_EQ(ed.text(), std::string("one Xtwo"));
    // Undo groups the typing run.
    ed.undo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("one two"));
    ed.redo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("one Xtwo"));
    // Unicode stepping.
    Editor u = fresh("a\xC3\xA9");
    key(u, ImGuiKey_End);
    key(u, ImGuiKey_Backspace);
    CHECK_EQ(u.text(), std::string("a"));
}

static void test_lists() {
    Editor ed = fresh("- one");
    key(ed, ImGuiKey_End);
    key(ed, ImGuiKey_Enter);
    type(ed, "two");
    CHECK_EQ(ed.text(), std::string("- one\n- two"));
    key(ed, ImGuiKey_Enter);
    key(ed, ImGuiKey_Enter);  // empty item ends the list
    CHECK_EQ(ed.text(), std::string("- one\n- two\n"));

    Editor o = fresh("1. a");
    key(o, ImGuiKey_End);
    key(o, ImGuiKey_Enter);
    CHECK_EQ(o.text(), std::string("1. a\n2. "));

    Editor t = fresh("- [x] done");
    key(t, ImGuiKey_End);
    key(t, ImGuiKey_Enter);
    CHECK_EQ(t.text(), std::string("- [x] done\n- [ ] "));

    Editor q = fresh("> quoted");
    key(q, ImGuiKey_End);
    key(q, ImGuiKey_Enter);
    CHECK_EQ(q.text(), std::string("> quoted\n> "));

    // Backspace right after a bullet removes the bullet.
    Editor b = fresh("- ");
    key(b, ImGuiKey_End);
    key(b, ImGuiKey_Backspace);
    CHECK_EQ(b.text(), std::string(""));

    // Tab on a list line indents the whole item.
    Editor i = fresh("- item");
    key(i, ImGuiKey_End);
    key(i, ImGuiKey_Tab);
    CHECK_EQ(i.text(), std::string("    - item"));
    key(i, ImGuiKey_Tab, ImGuiMod_Shift);
    CHECK_EQ(i.text(), std::string("- item"));

    Editor tt = fresh("buy milk");
    tt.toggle_task();
    frame(tt);
    CHECK_EQ(tt.text(), std::string("- [ ] buy milk"));
    tt.toggle_task();
    frame(tt);
    CHECK_EQ(tt.text(), std::string("- [x] buy milk"));
}

static void test_lines() {
    Editor ed = fresh("a\nb\nc");
    key(ed, ImGuiKey_DownArrow);
    ed.move_lines(-1);
    frame(ed);
    CHECK_EQ(ed.text(), std::string("b\na\nc"));
    CHECK_EQ(ed.cursor().line, 0);
    ed.move_lines(1);
    ed.move_lines(1);
    frame(ed);
    CHECK_EQ(ed.text(), std::string("a\nc\nb"));
    ed.duplicate_lines(1);
    frame(ed);
    CHECK_EQ(ed.text(), std::string("a\nc\nb\nb"));
    CHECK_EQ(ed.cursor().line, 3);
    ed.delete_lines();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("a\nc\nb"));
    // Moving the first line up is a no-op.
    Editor f = fresh("x\ny");
    f.move_lines(-1);
    CHECK_EQ(f.text(), std::string("x\ny"));
}

static void test_wrap_and_clipboard() {
    Editor ed = fresh("make this bold");
    ed.select_range({0, 5}, {0, 9});
    frame(ed);
    ed.wrap_selection("**");
    frame(ed);
    CHECK_EQ(ed.text(), std::string("make **this** bold"));
    CHECK_EQ(ed.selected_text(), std::string("this"));
    ed.wrap_selection("**");  // toggles back off
    frame(ed);
    CHECK_EQ(ed.text(), std::string("make this bold"));

    // Typing a pair character around a selection wraps it.
    ed.select_range({0, 0}, {0, 4});
    frame(ed);
    type(ed, "`");
    CHECK_EQ(ed.text(), std::string("`make` this bold"));

    // Whole-line copy pastes as a line above.
    Editor c = fresh("first\nsecond");
    c.copy();
    key(c, ImGuiKey_DownArrow);
    c.paste();
    frame(c);
    CHECK_EQ(c.text(), std::string("first\nfirst\nsecond"));
}

static void test_vertical_and_wrap() {
    // A long line wraps; down arrow moves by visual row, not by line.
    std::string long_line;
    for (int i = 0; i < 40; i++) long_line += "word ";
    Editor ed = fresh(long_line + "\nnext");
    key(ed, ImGuiKey_DownArrow);
    CHECK_EQ(ed.cursor().line, 0);
    CHECK(ed.cursor().col > 0);
    key(ed, ImGuiKey_End);
    key(ed, ImGuiKey_End);
    CHECK_EQ(ed.cursor().col, (int)long_line.size());
    key(ed, ImGuiKey_End, ImGuiMod_Ctrl);
    CHECK_EQ(ed.cursor().line, 1);
    key(ed, ImGuiKey_Home, ImGuiMod_Ctrl);
    CHECK_EQ(ed.cursor().line, 0);
    CHECK_EQ(ed.cursor().col, 0);
}

static void test_reader() {
    Editor ed = fresh("# T\n\n- [ ] task\n\n```\ncode\n```");
    ed.set_reader(true);
    frame(ed);
    type(ed, "zzz");  // reader is read-only
    CHECK_EQ(ed.text(), std::string("# T\n\n- [ ] task\n\n```\ncode\n```"));
    ed.set_reader(false);
    frame(ed);
    CHECK_EQ(ed.headings().size(), (size_t)1);
}

static void test_fuzzy_and_keys() {
    // The palette filters with fuzzy_match and highlights the positions it
    // returns: a subsequence (any case) matches, anything else doesn't, and
    // the positions point at the matched characters, in order.
    int score;
    CHECK(fuzzy_match("", "anything", score, nullptr));
    CHECK(!fuzzy_match("xyz", "delta roadmap", score, nullptr));
    CHECK(!fuzzy_match("pamdaor", "delta roadmap", score, nullptr));  // letters out of order
    auto low = [](char ch) { return (char)std::tolower((unsigned char)ch); };
    std::string wrong;  // patterns whose match or positions are wrong
    for (const char* q : {"drm", "ROAD", "d r", "Delta Roadmap"}) {
        std::string t = "delta roadmap";
        std::vector<int> pos;
        bool ok = fuzzy_match(q, t, score, &pos);
        std::string want;
        for (const char* p = q; *p; p++)
            if (*p != ' ') want += low(*p);
        if (std::string(q) == "Delta Roadmap") want = "delta roadmap";  // contiguous: spaces are matched too
        ok = ok && pos.size() == want.size();
        for (size_t i = 0; ok && i < pos.size(); i++)
            ok = pos[i] >= 0 && pos[i] < (int)t.size() && (i == 0 || pos[i] > pos[i - 1]) && low(t[pos[i]]) == want[i];
        if (!ok) wrong += std::string("[") + q + "] ";
    }
    CHECK_EQ(wrong, std::string());
    // The obvious candidate ranks first.
    auto best = [](const std::string& q, const std::vector<std::string>& labels) {
        std::string top;
        int top_s = 0;
        bool any = false;
        for (auto& l : labels) {
            int s;
            if (fuzzy_match(q, l, s, nullptr) && (!any || s > top_s)) { top = l; top_s = s; any = true; }
        }
        return top;
    };
    CHECK_EQ(best("road", {"r o a d", "delta roadmap"}), std::string("delta roadmap"));  // contiguous beats scattered
    CHECK_EQ(best("note", {"denotes", "note taking"}), std::string("note taking"));      // word start beats mid-word
    CHECK_EQ(best("map", {"bitmaps and heaps", "map"}), std::string("map"));             // exact name first

    // Chords: what the parser reads is the key and modifiers meant...
    Chord c;
    CHECK(parse_chord("ctrl+shift+p", c) && c.key == ImGuiKey_P && c.mods == (ImGuiMod_Ctrl | ImGuiMod_Shift));
    CHECK(parse_chord("ctrl+,", c) && c.key == ImGuiKey_Comma && c.mods == ImGuiMod_Ctrl);
    // ...however it's spelled...
    auto same = [](const char* a, const char* b) {
        Chord x, y;
        return parse_chord(a, x) && parse_chord(b, y) && x.key == y.key && x.mods == y.mods;
    };
    CHECK(same("Ctrl+Shift+P", "ctrl+shift+p"));
    CHECK(same("shift+ctrl+p", "ctrl+shift+p"));
    CHECK(same("cmd+s", "ctrl+s"));
    CHECK(same("ctrl++", "ctrl+="));
    CHECK(same("  alt+up ", "alt+up"));
    // ...and every chord delta writes out reads back as itself.
    std::string no_round_trip;
    for (const char* s : {"ctrl+shift+p", "alt+up", "ctrl+,", "ctrl+=", "f12", "super+0", "ctrl+shift+alt+z",
                          "numpad+", "escape", "ctrl+[", "shift+tab", "ctrl+pagedown", "ctrl+\\", "ctrl+/"}) {
        Chord a, b;
        if (!(parse_chord(s, a) && parse_chord(chord_str(a), b) && a.key == b.key && a.mods == b.mods)) no_round_trip += std::string(s) + " ";
    }
    CHECK_EQ(no_round_trip, std::string());
    // Nonsense is refused, not bound to something else.
    std::string accepted;
    for (const char* s : {"", "hyper+q", "ctrl+", "ctrl+nosuchkey", "f13", "ctrl+shift"})
        if (parse_chord(s, c)) accepted += std::string("[") + s + "] ";
    CHECK_EQ(accepted, std::string());
    std::vector<std::string> bad;
    auto list = parse_chords("ctrl+z  ctrl+shift+z nope+x", &bad);
    CHECK_EQ((int)list.size(), 2);
    CHECK(bad == std::vector<std::string>{"nope+x"});
}

static void test_ini() {
    // The editor indexes the style vector by byte, so it must cover every
    // byte of any line, however malformed, and set it all in mono.
    LineInfo info;
    std::vector<uint16_t> st;
    std::string short_or_not_mono;
    for (const char* s : {"", "   ", "[", "[x", "]", "=", "= ;", " ; c", "a=b ;c", "[editor] ; c", "k = #12345"}) {
        ini_analyze(s, info, st);
        bool mono = std::all_of(st.begin(), st.end(), [](uint16_t f) { return (f & S_CodeBlock) != 0; });
        if (st.size() != std::string(s).size() || !mono) short_or_not_mono += std::string("[") + s + "] ";
    }
    CHECK_EQ(short_or_not_mono, std::string());

    // It reads a line the way the config parser does: key, value, and a
    // " ; comment" tail, but a ';' inside a value and a '#' colour are values.
    ini_analyze("font_size = 14 ; px", info, st);
    CHECK((st[0] & I_Key) && (st[12] & I_Value) && (st[17] & I_Comment) && !(st[17] & I_Value));
    ini_analyze("daily_format = %Y;%m", info, st);
    CHECK((st[17] & I_Value) && !(st[17] & I_Comment));  // the ';'
    ini_analyze("accent = #e6e6e6", info, st);
    CHECK((st[9] & I_Value) && (st[9] & I_Color) && !(st[9] & I_Comment));
    ini_analyze(";font_size = 14", info, st);
    CHECK((st[5] & I_Comment) && !(st[5] & I_Key));

    // Section headers, for the outline.
    std::string name;
    CHECK(ini_section("  [keys]  ", name));
    CHECK_EQ(name, std::string("keys"));
    CHECK(!ini_section("key = [x]", name));
}

static void test_config_schema() {
    const Config def = [] {
        Config c;
        std::vector<ConfigIssue> is;
        parse_config("", c, is);
        return c;
    }();

    // Every setting accepts its own default in either theme, and the file
    // written on first launch parses cleanly: otherwise a fresh install
    // shows errors the user never made.
    std::string rejected;  // settings whose default is flagged
    for (auto& s : all_settings())
        for (const char* theme : {"dark", "light"}) {
            Config c;
            std::vector<ConfigIssue> is;
            parse_config(std::string("[") + s.section + "]\n" + s.key + " = " + s.default_for(theme) + "\n", c, is);
            if (!is.empty()) rejected += std::string(s.key) + "/" + theme + " ";
        }
    CHECK_EQ(rejected, std::string());
    {
        Config c;
        std::vector<ConfigIssue> is;
        parse_config(default_config_text(), c, is);
        CHECK(is.empty());
    }

    // Good values apply; bad ones fall back (or clamp) without taking the
    // rest of the file down, and each is reported on its own line.
    Config cfg;
    std::vector<ConfigIssue> issues;
    parse_config("; comment\n"                    // 0
                 "[editor]\n"                     // 1
                 "font_size = 18 ; px\n"          // 2  inline comment isn't part of the value
                 "tab_size = 99\n"                // 3  out of range: clamped
                 "line_numbers = maybe\n"         // 4  unusable: default
                 "line_hieght = 2\n"              // 5  typo
                 "caret_style = BLOCK\r\n"        // 6  any case, CRLF line ending
                 "[Appearance]\n"                 // 7
                 "theme = light\n"                // 8
                 "column_width = 90\n"            // 9  wrong section: not applied
                 "[colors]\n"                     // 10
                 "accent = #123456\n"             // 11 '#' starts a colour, not a comment
                 "text = orange\n"                // 12 not a colour: no override
                 "[keys]\n"                       // 13
                 "edit.undo = ctrl+u\n"           // 14
                 "[general]\n"                    // 15
                 "daily_space = \"my journal\"\n" // 16 quotes are stripped
                 "daily_format = %Y;%m\n",        // 17 ';' without a space is kept
                 cfg, issues);
    CHECK(cfg.font_size == 18.0f);
    CHECK_EQ(cfg.tab_size, (int)find_setting("editor", "tab_size")->hi);
    CHECK_EQ(cfg.line_numbers, def.line_numbers);
    CHECK_EQ(cfg.caret_style, std::string("block"));
    CHECK_EQ(cfg.theme, std::string("light"));
    CHECK_EQ(cfg.column_width, def.column_width);
    CHECK_EQ(cfg.colors["accent"], std::string("#123456"));
    CHECK(!cfg.colors.count("text"));
    CHECK_EQ(cfg.keys["edit.undo"], std::string("ctrl+u"));
    CHECK_EQ(cfg.daily_space, std::string("my journal"));
    CHECK_EQ(cfg.daily_format, std::string("%Y;%m"));
    std::vector<int> lines;
    for (auto& i : issues) lines.push_back(i.line);
    CHECK(lines == (std::vector<int>{3, 4, 5, 9, 12}));
    auto msg = [&](int line) {
        for (auto& i : issues)
            if (i.line == line) return i.msg;
        return std::string();
    };
    CHECK(msg(5).find("line_height") != std::string::npos);  // points at the fix
    CHECK(msg(9).find("[editor]") != std::string::npos);

    // The live config: deleting a setting reverts it to its default.
    parse_config("[keys]\n", cfg, issues);
    CHECK(cfg.font_size == def.font_size && cfg.theme == def.theme && cfg.colors.empty() && cfg.keys.empty());

    // Garbage never crashes and never leaves a number outside its range.
    std::string junk = "[editor]\n=\n= 5\nfont_size\nfont_size =\nline_height = 1e999\ncolumn_width = 99999999999999999999\n"
                       "tab_size = -3\n[\n]\n[editor\nautosave_delay = inf\n";
    junk += std::string("font_size = \0\x01\xff", 15) + "\n" + std::string(100000, 'x') + " = 1\n";
    Config g;
    std::vector<ConfigIssue> gi;
    parse_config(junk, g, gi);
    CHECK(!gi.empty());
    std::string out_of_range;
    for (auto& s : all_settings()) {
        if (s.kind != Setting::Kind::Float && s.kind != Setting::Kind::Int) continue;
        float v = std::string(s.key) == "font_size" ? g.font_size
                : std::string(s.key) == "line_height" ? g.line_height
                : std::string(s.key) == "column_width" ? (float)g.column_width
                : std::string(s.key) == "tab_size" ? (float)g.tab_size
                : std::string(s.key) == "autosave_delay" ? g.autosave_delay
                : std::string(s.key) == "blank_line_height" ? g.blank_line_height
                : std::string(s.key) == "animation_speed" ? g.animation_speed : s.lo;
        if (!(v >= s.lo && v <= s.hi)) out_of_range += std::string(s.key) + " ";
    }
    CHECK_EQ(out_of_range, std::string());

    // From disk: a missing file is reported and changes nothing; a real one
    // (CRLF, as Windows editors save it) loads.
    fs::path p = fs::temp_directory_path() / "delta_test_config.ini";
    fs::remove(p);
    Config keep;
    keep.font_size = 21;
    CHECK(!load_config(p, keep, issues));
    CHECK(keep.font_size == 21.0f);
    atomic_write(p, "[editor]\r\nfont_size = 20\r\n[appearance]\r\ntheme = light\r\n", nullptr);
    Config disk;
    std::vector<ConfigIssue> di;
    CHECK(load_config(p, disk, di));
    CHECK(di.empty() && disk.font_size == 20.0f && disk.theme == "light");
    fs::remove(p);

    // "Add a setting": wherever config_insert_line puts a new line, it must
    // land in its own section, so the value takes effect.
    struct Add { const char *section, *key, *value; };
    const Add adds[] = {{"general", "daily_space", "journal"}, {"editor", "tab_size", "2"}, {"appearance", "theme", "light"},
                        {"colors", "accent", "#123456"}, {"keys", "edit.undo", "ctrl+u"}};
    auto split = [](const std::string& t) {
        std::vector<std::string> out(1);
        for (char ch : t)
            if (ch == '\n') out.emplace_back();
            else out.back().push_back(ch);
        return out;
    };
    for (auto doc : {split(default_config_text()), std::vector<std::string>{""},
                     std::vector<std::string>{"[keys]", "", "; note", "[editor]", "font_size = 15", "; tabs", "", "[general]"}}) {
        for (auto& a : adds) {
            bool header = false;
            int at = config_insert_line(doc, a.section, header);
            std::vector<std::string> ins;
            if (header) ins.push_back(std::string("[") + a.section + "]");
            ins.push_back(std::string(a.key) + " = " + a.value);
            doc.insert(doc.begin() + at, ins.begin(), ins.end());
        }
        std::string text;
        for (auto& l : doc) text += l + "\n";
        Config c;
        std::vector<ConfigIssue> is;
        parse_config(text, c, is);
        CHECK(is.empty());
        CHECK(c.daily_space == "journal" && c.tab_size == 2 && c.theme == "light" && c.colors["accent"] == "#123456" &&
              c.keys["edit.undo"] == "ctrl+u");
    }

    // App state (last note, window size) survives a save and load.
    fs::path kvp = fs::temp_directory_path() / "delta_test_state" / "state.txt";
    fs::remove_all(kvp.parent_path());
    std::map<std::string, std::string> kv = {{"last_note", "C:/My Notes/a = b.md"}, {"window", "1280 800"}, {"empty", ""}};
    save_kv(kvp, kv);  // creates the folder
    CHECK(load_kv(kvp) == kv);
    fs::remove_all(kvp.parent_path());
}

#include "store.h"

// Notes are the user's documents: every store operation must keep their
// bytes intact and refuse anything that would overwrite or orphan one.
static void test_store() {
    fs::path root = fs::temp_directory_path() / "delta_test_store";
    fs::remove_all(root);
    Store st;
    st.set_root(root);
    auto body = [](const NoteRef& n) {
        std::string s;
        return read_file(n.path, s) ? s : std::string("<unreadable>");
    };

    // Content round-trips byte for byte: UTF-8, CRLF, no final newline.
    const std::string text = "# Caf\xC3\xA9 \xE2\x98\x95\r\n\r\n- [ ] item\n\ttab\n\n\nlast line";
    NoteRef a;
    CHECK_EQ(st.create_note("work", "Caf\xC3\xA9 notes", text, a), std::string());
    CHECK_EQ(body(a), text);
    CHECK(st.exists("work", "Caf\xC3\xA9 notes"));
    auto listed = st.notes("work");
    CHECK(listed.size() == 1 && listed[0].name == "Caf\xC3\xA9 notes" && listed[0].path == a.path);

    // Saving again replaces the content and leaves nothing else behind.
    std::string err;
    CHECK(atomic_write(a.path, "v2", &err));
    CHECK_EQ(body(a), std::string("v2"));
    int files = 0;
    for (auto& e : fs::directory_iterator(root / "work")) files += e.is_regular_file();
    CHECK_EQ(files, 1);
    CHECK(atomic_write(a.path, "", &err) && body(a).empty());  // an emptied note stays a file
    CHECK(atomic_write(a.path, text, &err));

    // Nothing overwrites an existing note.
    NoteRef b, dup;
    CHECK_EQ(st.create_note("work", "plan", "plan text", b), std::string());
    CHECK(!st.create_note("work", "  plan ", "other", dup).empty());
    CHECK_EQ(body(b), std::string("plan text"));
    NoteRef a2 = a;
    CHECK(!st.rename_note(a2, "plan").empty());
    CHECK(body(a) == text && body(b) == "plan text");
    NoteRef c;
    CHECK_EQ(st.create_note("home", "plan", "home plan", c), std::string());
    NoteRef b2 = b;
    CHECK(!st.move_note(b2, "home").empty());
    CHECK(body(b) == "plan text" && body(c) == "home plan");
    CHECK(!st.rename_space("home", "work").empty());
    CHECK(body(b) == "plan text" && body(c) == "home plan");
    CHECK(!st.delete_space("work").empty());  // not empty
    CHECK(body(a) == text && body(b) == "plan text");

    // Names can't reach outside the notes folder or hide from the listing.
    NoteRef bad;
    std::string accepted;
    for (const char* n : {"", "   ", "../escape", "..\\escape", "a/b", ".hidden", "trailing.", "c:x", "tab\there"})
        if (st.create_note("work", n, "x", bad).empty()) accepted += std::string("[") + n + "] ";
    CHECK_EQ(accepted, std::string());
    CHECK(!st.ensure_space("../outside").empty());
    CHECK(!fs::exists(root.parent_path() / "escape.md") && !fs::exists(root / "escape.md"));

    // Renames and moves carry the content with them.
    CHECK_EQ(st.rename_note(b, "roadmap"), std::string());
    CHECK(b.name == "roadmap" && body(b) == "plan text" && !st.exists("work", "plan"));
    CHECK_EQ(st.rename_note(b, "Roadmap"), std::string());  // case only
    CHECK(b.name == "Roadmap" && body(b) == "plan text");
    CHECK_EQ(st.move_note(b, "home"), std::string());
    CHECK(b.space == "home" && body(b) == "plan text" && !st.exists("work", "Roadmap"));
    CHECK_EQ(st.rename_space("home", "house"), std::string());
    CHECK(body(st.ref("house", "Roadmap")) == "plan text" && body(st.ref("house", "plan")) == "home plan");

    // Deleting goes to the trash, and two deleted notes with the same name
    // both survive there. The trash isn't a space.
    NoteRef t1, t2;
    CHECK_EQ(st.create_note("work", "draft", "first draft", t1), std::string());
    CHECK_EQ(st.trash_note(t1), std::string());
    CHECK_EQ(st.create_note("work", "draft", "second draft", t2), std::string());
    CHECK_EQ(st.trash_note(t2), std::string());
    CHECK(!st.exists("work", "draft"));
    std::vector<std::string> trashed;
    for (auto& e : fs::directory_iterator(root / ".trash" / "work")) {
        std::string s;
        read_file(e.path(), s);
        trashed.push_back(s);
    }
    std::sort(trashed.begin(), trashed.end());
    CHECK(trashed == (std::vector<std::string>{"first draft", "second draft"}));
    CHECK(st.spaces() == (std::vector<std::string>{"house", "work"}));
    CHECK_EQ((int)st.all_notes().size(), 3);  // Café notes, Roadmap, plan

    // Only .md files are notes; an empty space can go.
    atomic_write(root / "work" / "scratch.txt", "x", nullptr);
    CHECK_EQ(st.note_count("work"), 1);
    CHECK_EQ(st.ensure_space("empty"), std::string());
    CHECK_EQ(st.delete_space("empty"), std::string());
    CHECK(!fs::exists(root / "empty"));

    fs::remove_all(root);
}

// Every image is 800x400 except those named "missing".
struct FakeImages : ImageSource {
    bool size(const std::string& src, int& w, int& h) override {
        if (src.find("missing") != std::string::npos) return false;
        w = 800;
        h = 400;
        return true;
    }
    ImTextureID texture(const std::string&, int, float& shown) override {
        shown = 1.0f;
        return ImTextureID{};
    }
};

static void test_images() {
    // What counts as an image line.
    std::string alt, src;
    CHECK(md_image_line("![a cat](cat.png)", &alt, &src));
    CHECK_EQ(alt, std::string("a cat"));
    CHECK_EQ(src, std::string("cat.png"));
    CHECK(md_image_line("  ![](../.delta/images/ab12.png)  ", &alt, &src));
    CHECK_EQ(src, std::string("../.delta/images/ab12.png"));
    CHECK(md_image_line("![x](<my pics/cat 1.png> \"title\")", &alt, &src));
    CHECK_EQ(src, std::string("my pics/cat 1.png"));
    CHECK(md_image_line("![x](cat.png \"a title\")", &alt, &src));
    CHECK_EQ(src, std::string("cat.png"));
    CHECK(md_image_line("![[nested] alt](cat.png)", &alt, &src));
    CHECK_EQ(alt, std::string("[nested] alt"));
    CHECK(!md_image_line("see ![a](cat.png)"));        // text around it: stays text
    CHECK(!md_image_line("![a](cat.png) more"));
    CHECK(!md_image_line("![a]()"));
    CHECK(!md_image_line("[a](cat.png)"));             // a link, not an image
    LineInfo info;
    std::vector<uint16_t> st;
    md_analyze("![](a.png)", false, info, st);
    CHECK(info.kind == LineKind::Image);
    md_analyze("![](a.png)", true, info, st);
    CHECK(info.kind == LineKind::CodeBlock);           // not inside code

    // Hashes, the store, and finding images from a note.
    CHECK_EQ(sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex(std::string(1000, 'a')).substr(0, 16), std::string("41edece42d63e8d9"));
    fs::path root = fs::temp_directory_path() / "delta_test_images";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    ImageStore store;
    store.set_root(root);
    std::string err;
    std::string link = store.add("fake png bytes", "PNG", &err);
    CHECK(err.empty());
    CHECK_EQ(link, "../.delta/images/" + sha256_hex("fake png bytes").substr(0, 32) + ".png");
    CHECK_EQ(store.add("fake png bytes", "png"), link);  // same bytes, same file
    CHECK(store.add("other bytes", "png") != link);
    int files = 0;
    for (auto& f : fs::directory_iterator(store.dir())) { (void)f; files++; }
    CHECK_EQ(files, 2);
    fs::path note_dir = root / "space";
    CHECK(resolve_image(note_dir, link) == (root / ".delta" / "images" / link.substr(17)).lexically_normal());
    CHECK(resolve_image(note_dir, "https://example.com/a.png").empty());
    CHECK(resolve_image(note_dir, "my%20cat.png") == (note_dir / "my cat.png").lexically_normal());
    fs::remove_all(root, ec);

    // Downscaling averages: a 2x2 checker of black and white becomes grey,
    // and a transparent pixel's colour doesn't bleed in.
    const uint8_t px[] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    std::vector<uint8_t> out;
    resample_rgba(px, 2, 2, 1, 1, out);
    CHECK(std::abs(out[0] - 128) <= 1 && out[3] == 255);
    const uint8_t half[] = {200, 0, 0, 255, 0, 255, 0, 0};  // red, and a transparent "green"
    resample_rgba(half, 2, 1, 1, 1, out);
    CHECK(out[0] == 200 && out[1] == 0 && std::abs(out[3] - 128) <= 1);

    // In the editor: the caret never rests on an image; arrows pick it whole.
    FakeImages fake;
    g_images = &fake;
    Editor ed = fresh("para\n![](a.png)\nafter");
    ed.select_range({0, 4}, {0, 4});
    frame(ed);
    auto picked = [](Editor& e, int line) { return e.has_selection() && e.image_for_caret() == line; };
    key(ed, ImGuiKey_RightArrow);
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_RightArrow);
    CHECK(!ed.has_selection() && ed.cursor().line == 2 && ed.cursor().col == 0);
    key(ed, ImGuiKey_LeftArrow);
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_LeftArrow);
    CHECK(!ed.has_selection() && ed.cursor().line == 0 && ed.cursor().col == 4);
    key(ed, ImGuiKey_DownArrow);
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_DownArrow);
    CHECK(!ed.has_selection() && ed.cursor().line == 2);
    CHECK_EQ(ed.word_count(), 2);  // the image isn't words

    // A caret put on its line (a click beside it, a jump) picks it.
    ed.select_range({1, 0}, {1, 0});
    frame(ed);
    CHECK(picked(ed, 1));

    // Typing or enter with it picked starts a line of its own after it; tab
    // leaves it be.
    key(ed, ImGuiKey_Tab);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nafter"));
    type(ed, "x");
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    ed.select_range({1, 0}, {1, 0});
    frame(ed);
    key(ed, ImGuiKey_Enter);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\n\nx\nafter"));
    CHECK(!ed.has_selection() && ed.cursor().line == 2 && ed.cursor().col == 0);

    // Backspace on an empty line below takes the line and picks the image;
    // the next one removes it (its line is left empty); undo brings it back.
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n\nx\nafter"));
    CHECK(!ed.has_selection() && ed.cursor().line == 1);
    ed.undo();
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    // Below it on a line with text, backspace just picks it.
    ed.select_range({2, 0}, {2, 0});
    frame(ed);
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    CHECK(picked(ed, 1));

    // Delete on a picked image removes it too; from an empty line above,
    // that line goes and the image is picked.
    Editor e2 = fresh("![](a.png)");
    e2.select_range({0, 0}, {0, 0});
    frame(e2);
    key(e2, ImGuiKey_Delete);
    CHECK_EQ(e2.text(), std::string(""));
    Editor e3 = fresh("\n![](a.png)");
    e3.select_range({0, 0}, {0, 0});
    frame(e3);
    key(e3, ImGuiKey_Delete);
    CHECK_EQ(e3.text(), std::string("![](a.png)"));
    CHECK(picked(e3, 0));

    // An image that can't be shown stays ordinary text.
    Editor e4 = fresh("![](missing.png)");
    e4.select_range({0, 0}, {0, 0});
    frame(e4);
    key(e4, ImGuiKey_RightArrow);
    CHECK_EQ(e4.cursor().col, 1);

    // Pasted or dropped images arrive as whole lines: onto an empty line,
    // after the caret's line, or splitting it; one undo takes them back.
    Editor e5 = fresh("one\n\nthree");
    e5.select_range({1, 0}, {1, 0});
    frame(e5);
    e5.insert_lines({"![](a.png)", "![](b.png)"});
    CHECK_EQ(e5.text(), std::string("one\n![](a.png)\n![](b.png)\nthree"));
    CHECK(e5.cursor().line == 2 && e5.cursor().col == (int)std::string("![](b.png)").size());
    e5.undo();
    CHECK_EQ(e5.text(), std::string("one\n\nthree"));
    e5.select_range({0, 2}, {0, 2});  // mid-word: the line splits around it
    frame(e5);
    e5.insert_lines({"![](a.png)"});
    CHECK_EQ(e5.text(), std::string("on\n![](a.png)\ne\n\nthree"));
    e5.select_range({4, 5}, {4, 5});
    frame(e5);
    e5.insert_lines({"![](c.png)"});
    CHECK_EQ(e5.text(), std::string("on\n![](a.png)\ne\n\nthree\n![](c.png)"));
    frame(e5);
    CHECK_EQ(e5.image_for_caret(), 5);  // the caret is left beside it
    CHECK_EQ(e5.image_src(5), std::string("c.png"));
    e5.select_range({5, 0}, {5, 10});
    frame(e5);
    CHECK_EQ(e5.image_for_caret(), 5);  // a selection of exactly the image
    e5.select_range({4, 0}, {5, 10});
    frame(e5);
    CHECK_EQ(e5.image_for_caret(), -1);

    // A download landing swaps its placeholder everywhere, undo history too.
    Editor e6 = fresh("x");
    e6.select_range({0, 1}, {0, 1});
    frame(e6);
    e6.insert_lines({"![](delta-pending:1)"});
    CHECK(e6.replace_line_everywhere("![](delta-pending:1)", "![](real.png)"));
    CHECK_EQ(e6.text(), std::string("x\n![](real.png)"));
    e6.undo();
    CHECK_EQ(e6.text(), std::string("x"));
    e6.redo();
    CHECK_EQ(e6.text(), std::string("x\n![](real.png)"));
    CHECK(!e6.replace_line_everywhere("![](delta-pending:1)", "y"));
    g_images = nullptr;

    // Sizes, the way Obsidian writes them, and rows of images.
    std::vector<ImageRef> refs;
    CHECK(md_image_row("![cat|300](a.png)", &refs));
    CHECK(refs.size() == 1 && refs[0].alt == "cat" && refs[0].w == 300 && refs[0].h == 0);
    CHECK(md_image_row("![|300x200](a.png)", &refs) && refs[0].w == 300 && refs[0].h == 200);
    CHECK(md_image_row("![a|b](a.png)", &refs) && refs[0].alt == "a|b" && refs[0].w == 0);  // not a size
    CHECK(md_image_row("![](a.png) ![](b.png)  ![x](<c d.png>)", &refs));
    CHECK(refs.size() == 3 && refs[1].src == "b.png" && refs[2].src == "c d.png");
    CHECK(!md_image_row("![](a.png)![](b.png)"));      // they need space between
    CHECK(!md_image_row("![](a.png) and ![](b.png)"));
    CHECK_EQ(md_image_resized("![cat|300](a.png \"t\")", 120), std::string("![cat|120](a.png \"t\")"));
    CHECK_EQ(md_image_resized("![cat|300](a.png)", 0), std::string("![cat](a.png)"));
    CHECK_EQ(md_image_resized("![](a.png)", 50), std::string("![|50](a.png)"));
    CHECK_EQ(md_image_text("x", "my pic.png", 200), std::string("![x|200](<my pic.png>)"));

    g_images = &fake;
    {
        // |300: 300 px wide (at 1:1), height from the 2:1 image.
        Editor s = fresh("![|300](a.png)");
        ImVec2 a, b;
        CHECK(s.image_rect(0, 0, a, b));
        CHECK(std::fabs((b.x - a.x) - 300) < 1 && std::fabs((b.y - a.y) - 150) < 1);

        // A row: justified, one height, exactly the column's width.
        Editor r = fresh("![](a.png) ![](b.png) ![](c.png)");
        ImVec2 a0, b0, a2, b2;
        CHECK(r.image_rect(0, 0, a0, b0) && r.image_rect(0, 2, a2, b2));
        CHECK(std::fabs(a0.y - a2.y) < 0.5f && std::fabs((b0.y - a0.y) - (b2.y - a2.y)) < 1);
        CHECK(std::fabs((b2.x - a0.x) - 600) < 1.5f);
        CHECK_EQ((int)r.image_list().size(), 3);
        // Five wrap into two rows.
        Editor r5 = fresh("![](a.png) ![](b.png) ![](c.png) ![](d.png) ![](e.png)");
        ImVec2 p0, q0, p4, q4;
        CHECK(r5.image_rect(0, 0, p0, q0) && r5.image_rect(0, 4, p4, q4));
        CHECK(p4.y > q0.y);

        // Arrows pick image by image; backspace takes the picked one out and
        // picks the one before, delete the one that takes its place.
        r.select_range({0, 0}, {0, 0});
        frame(r);
        int t = -1;
        CHECK(r.image_for_caret(&t) == 0 && t == 0);
        key(r, ImGuiKey_RightArrow);
        CHECK(r.image_for_caret(&t) == 0 && t == 1);
        key(r, ImGuiKey_RightArrow);
        CHECK(r.image_for_caret(&t) == 0 && t == 2);
        key(r, ImGuiKey_LeftArrow);
        key(r, ImGuiKey_Backspace);
        CHECK_EQ(r.text(), std::string("![](a.png) ![](c.png)"));
        CHECK(r.image_for_caret(&t) == 0 && t == 0);
        Editor rd = fresh("![](a.png) ![](b.png) ![](c.png)");
        rd.select_range({0, 11}, {0, 21});  // b
        frame(rd);
        key(rd, ImGuiKey_Delete);
        CHECK_EQ(rd.text(), std::string("![](a.png) ![](c.png)"));
        CHECK(rd.image_for_caret(&t) == 0 && t == 1);
        // Up and down go between the rows a long row wraps into (3 + 2):
        // the nearest image below.
        r5.select_range({0, 0}, {0, 0});
        frame(r5);
        key(r5, ImGuiKey_DownArrow);
        CHECK(r5.image_for_caret(&t) == 0 && t == 3);
        key(r5, ImGuiKey_UpArrow);
        CHECK(r5.image_for_caret(&t) == 0 && t == 0);
        // Stepping along a row in view doesn't scroll: it keeps the picked
        // image in view, not the line's whole height or the image after it.
        std::string above;
        for (int k = 0; k < 40; k++) above += "line\n";
        Editor sv = fresh(above + "![](a.png) ![](b.png) ![](c.png) ![](d.png) ![](e.png)\nend");
        sv.select_range({40, 0}, {40, 0});
        frame(sv);
        float s0 = sv.scroll();
        key(sv, ImGuiKey_RightArrow);
        key(sv, ImGuiKey_RightArrow);
        CHECK(sv.image_for_caret(&t) == 40 && t == 2);
        CHECK(std::fabs(sv.scroll() - s0) < 0.5f);
        // ctrl+alt+right swaps the image the caret is on with the next; at
        // the end of the row there's nowhere to go.
        Editor nr = fresh("![](a.png) ![](b.png) ![](c.png)");
        nr.select_range({0, 21}, {0, 21});  // just after b: on b
        frame(nr);
        key(nr, ImGuiKey_RightArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
        CHECK_EQ(nr.text(), std::string("![](a.png) ![](c.png) ![](b.png)"));
        CHECK_EQ(nr.cursor().col, 32);
        key(nr, ImGuiKey_RightArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
        CHECK_EQ(nr.text(), std::string("![](a.png) ![](c.png) ![](b.png)"));
        key(nr, ImGuiKey_LeftArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
        key(nr, ImGuiKey_LeftArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
        CHECK_EQ(nr.text(), std::string("![](b.png) ![](a.png) ![](c.png)"));

        // alt+up/down with an image picked moves the image one step: out of
        // its row onto a line of its own, into the row beside, between the
        // rows a line wraps into; alone next to text, its line moves.
        auto pick_at = [&](Editor& e, int line, int b0, int b1) {
            e.select_range({line, b0}, {line, b1});
            frame(e);
        };
        Editor mv = fresh("![](a.png) ![](b.png) ![](c.png)\ntext");
        pick_at(mv, 0, 11, 21);  // b
        mv.move_lines(-1);
        frame(mv);
        CHECK_EQ(mv.text(), std::string("![](b.png)\n![](a.png) ![](c.png)\ntext"));
        CHECK(mv.image_for_caret(&t) == 0 && t == 0 && mv.has_selection());
        mv.move_lines(1);  // into the row below: its start, keeping reading order
        frame(mv);
        CHECK_EQ(mv.text(), std::string("![](b.png) ![](a.png) ![](c.png)\ntext"));
        CHECK(mv.image_for_caret(&t) == 0 && t == 0);
        mv.move_lines(1);  // out below
        frame(mv);
        CHECK_EQ(mv.text(), std::string("![](a.png) ![](c.png)\n![](b.png)\ntext"));
        CHECK(mv.image_for_caret(&t) == 1 && t == 0);
        mv.move_lines(-1);  // into the row above: its end
        frame(mv);
        CHECK_EQ(mv.text(), std::string("![](a.png) ![](c.png) ![](b.png)\ntext"));
        CHECK(mv.image_for_caret(&t) == 0 && t == 2);
        mv.move_lines(1);  // out below again
        frame(mv);
        mv.move_lines(1);  // alone, next to text: the line moves
        frame(mv);
        CHECK_EQ(mv.text(), std::string("![](a.png) ![](c.png)\ntext\n![](b.png)"));
        CHECK(mv.image_for_caret(&t) == 2 && t == 0 && mv.has_selection());
        mv.undo();
        mv.undo();
        CHECK_EQ(mv.text(), std::string("![](a.png) ![](c.png) ![](b.png)\ntext"));

        Editor mw = fresh("![](a.png) ![](b.png) ![](c.png) ![](d.png) ![](e.png)");  // 3 + 2
        pick_at(mw, 0, 44, 54);  // e: second of the bottom row (middle 452)
        mw.move_lines(-1);       // into the top row, before c (middle 500)
        frame(mw);
        CHECK_EQ(mw.text(), std::string("![](a.png) ![](b.png) ![](e.png) ![](c.png) ![](d.png)"));
        CHECK(mw.image_for_caret(&t) == 0 && t == 2);
        mw.move_lines(-1);  // the top row: out above
        frame(mw);
        CHECK_EQ(mw.text(), std::string("![](e.png)\n![](a.png) ![](b.png) ![](c.png) ![](d.png)"));
        CHECK(mw.image_for_caret(&t) == 0 && t == 0);

        // Typing between two images splits the row around the text.
        type(r, "hi");
        CHECK_EQ(r.text(), std::string("![](a.png)\nhi\n![](c.png)"));

        // Dragging an image onto the right half of another joins its row.
        Editor dr = fresh("![](a.png)\n\n![](b.png)");
        ImVec2 sa, sb, ta, tb;
        CHECK(dr.image_rect(0, 0, sa, sb) && dr.image_rect(2, 0, ta, tb));
        ImGuiIO& io = ImGui::GetIO();
        auto mouse = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(dr);
        };
        ImVec2 from((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f), to(tb.x - 20, (ta.y + tb.y) * 0.5f);
        mouse(from, false);
        mouse(from, true);
        for (int k = 1; k <= 6; k++) mouse(ImVec2(from.x + (to.x - from.x) * k / 6, from.y + (to.y - from.y) * k / 6), true);
        mouse(to, false);
        frame(dr);
        CHECK_EQ(dr.text(), std::string("\n![](b.png) ![](a.png)"));
        io.AddMousePosEvent(-1, -1);
        frame(dr);

        // In a line that wraps into rows, the space between them (and the
        // edges they share) joins the nearer row: only the grid's outer
        // edges mean a line of its own.
        Editor dw = fresh("![](a.png) ![](b.png) ![](c.png) ![](d.png) ![](e.png)");  // 3 + 2
        ImVec2 wa, wb, ea, eb;
        CHECK(dw.image_rect(0, 1, wa, wb) && dw.image_rect(0, 4, ea, eb));
        auto mouse_w = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(dw);
        };
        ImVec2 fw((ea.x + eb.x) * 0.5f, (ea.y + eb.y) * 0.5f), tw(wb.x - 10, wb.y + 2);  // just under b, its right half
        mouse_w(fw, false);
        mouse_w(fw, true);
        for (int k = 1; k <= 6; k++) mouse_w(ImVec2(fw.x + (tw.x - fw.x) * k / 6, fw.y + (tw.y - fw.y) * k / 6), true);
        mouse_w(tw, false);
        frame(dw);
        CHECK_EQ(dw.text(), std::string("![](a.png) ![](b.png) ![](e.png) ![](c.png) ![](d.png)"));
        io.AddMousePosEvent(-1, -1);
        frame(dw);

        // A click on an image picks it, whichever half it lands on.
        Editor dk = fresh("![](a.png) ![](b.png) ![](c.png)");
        CHECK(dk.image_rect(0, 1, sa, sb));
        auto click = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(dk);
        };
        ImVec2 left_half(sa.x + 5, (sa.y + sb.y) * 0.5f);
        click(left_half, false);
        click(left_half, true);
        click(left_half, false);
        CHECK(dk.has_selection() && dk.image_for_caret(&t) == 0 && t == 1);
        CHECK_EQ(dk.text(), std::string("![](a.png) ![](b.png) ![](c.png)"));
        io.AddMousePosEvent(-1, -1);
        frame(dk);

        // The blank space beside a resized image joins its row too.
        Editor db = fresh("![|200](a.png)\n\n![](b.png)");
        CHECK(db.image_rect(0, 0, ta, tb) && db.image_rect(2, 0, sa, sb));
        auto mouse4 = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(db);
        };
        ImVec2 f4((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f), t4(tb.x + 250, (ta.y + tb.y) * 0.5f);
        mouse4(f4, false);
        mouse4(f4, true);
        for (int k = 1; k <= 6; k++) mouse4(ImVec2(f4.x + (t4.x - f4.x) * k / 6, f4.y + (t4.y - f4.y) * k / 6), true);
        mouse4(t4, false);
        frame(db);
        CHECK_EQ(db.text(), std::string("![|200](a.png) ![](b.png)\n"));
        io.AddMousePosEvent(-1, -1);
        frame(db);

        // Escape mid-drag: it goes back, nothing changes.
        Editor dc = fresh("![](a.png)\n\n![](b.png)");
        CHECK(dc.image_rect(0, 0, sa, sb) && dc.image_rect(2, 0, ta, tb));
        auto mouse3 = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(dc);
        };
        ImVec2 f3((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f), t3(tb.x - 20, (ta.y + tb.y) * 0.5f);
        mouse3(f3, false);
        mouse3(f3, true);
        for (int k = 1; k <= 6; k++) mouse3(ImVec2(f3.x + (t3.x - f3.x) * k / 6, f3.y + (t3.y - f3.y) * k / 6), true);
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame(dc);
        io.AddKeyEvent(ImGuiKey_Escape, false);
        mouse3(t3, false);
        for (int k = 0; k < 30; k++) frame(dc);
        CHECK_EQ(dc.text(), std::string("![](a.png)\n\n![](b.png)"));
        io.AddMousePosEvent(-1, -1);
        frame(dc);

        // The handle: drag near half the column and it snaps there, written
        // as a width; a double-click puts it back to its own size.
        Editor rs = fresh("![](a.png)\n\ntext");
        ImVec2 ia, ib;
        CHECK(rs.image_rect(0, 0, ia, ib));  // 800 wide, capped to the 600 column
        auto mouse2 = [&](ImVec2 p, bool down) {
            io.AddMousePosEvent(p.x, p.y);
            io.AddMouseButtonEvent(0, down);
            frame(rs);
        };
        ImVec2 grip(ib.x - 1, (ia.y + ib.y) * 0.5f);
        mouse2(grip, false);
        mouse2(grip, false);
        mouse2(grip, true);
        for (int k = 1; k <= 5; k++) mouse2(ImVec2(grip.x - (grip.x - (ia.x + 304)) * k / 5, grip.y), true);
        mouse2(ImVec2(ia.x + 304, grip.y), false);
        CHECK_EQ(rs.text(), std::string("![|300](a.png)\n\ntext"));
        CHECK(rs.image_rect(0, 0, ia, ib));
        grip = ImVec2(ib.x - 1, (ia.y + ib.y) * 0.5f);
        g_time += 1.0;  // clear of the last click, for the double-click
        mouse2(grip, false);
        mouse2(grip, true);
        mouse2(grip, false);
        mouse2(grip, true);
        mouse2(grip, false);
        CHECK_EQ(rs.text(), std::string("![](a.png)\n\ntext"));
        io.AddMousePosEvent(-1, -1);
        frame(rs);
    }
    g_images = nullptr;

    // Clipboard formats: PNG and bitmaps round-trip, formats are told apart.
    std::vector<uint8_t> px2 = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255, 10, 20, 30, 0, 1, 2, 3, 255, 9, 9, 9, 9};  // 3x2
    std::string png = encode_png(px2, 3, 2);
    CHECK_EQ(image_ext(png), std::string("png"));
    std::vector<uint8_t> back;
    int bw = 0, bh = 0;
    CHECK(image_decode_bytes(png, back, bw, bh));
    CHECK(bw == 3 && bh == 2 && back == px2);
    std::string dib = rgba_to_dib(px2, 3, 2);
    CHECK(dib_to_rgba(dib, back, bw, bh));
    CHECK(bw == 3 && bh == 2 && back == px2);
    CHECK(image_ext("not an image").empty());
    // A 32-bit bitmap whose fourth byte is all zero is opaque, not invisible.
    std::vector<uint8_t> clear = px2;
    for (size_t i = 3; i < clear.size(); i += 4) clear[i] = 0;
    CHECK(dib_to_rgba(rgba_to_dib(clear, 3, 2), back, bw, bh));
    CHECK(back[3] == 255 && back[0] == 255);
}

static void test_smear() {
    const float dt = 1.0f / 60;
    // Trail: moving right, the right edge leads and the left edge lags, so
    // mid-flight the rect is stretched; then it settles to the exact target.
    SmearRect r;
    r.snap(ImVec2(0, 0), ImVec2(4, 20));
    r.set(ImVec2(200, 0), ImVec2(204, 20), true, 50.0f);
    for (int i = 0; i < 3; i++) r.update(dt);
    float width = r.corner(1).x - r.corner(0).x;
    CHECK(width > 20.0f);
    CHECK(!r.settled());
    for (int i = 0; i < 120; i++) r.update(dt);
    CHECK(r.settled());
    CHECK(std::fabs(r.corner(0).x - 200) < 0.5f && std::fabs(r.corner(2).x - 204) < 0.5f);

    // No trail: a plain glide keeps its shape the whole way.
    SmearRect g;
    g.snap(ImVec2(0, 0), ImVec2(4, 20));
    g.set(ImVec2(200, 0), ImVec2(204, 20), false, 50.0f);
    for (int i = 0; i < 3; i++) g.update(dt);
    CHECK(std::fabs((g.corner(1).x - g.corner(0).x) - 4.0f) < 0.5f);

    // Moving down: the bottom edge leads.
    SmearRect d;
    d.snap(ImVec2(0, 0), ImVec2(4, 20));
    d.set(ImVec2(0, 300), ImVec2(4, 320), true, 50.0f);
    for (int i = 0; i < 3; i++) d.update(dt);
    CHECK(d.corner(3).y - d.corner(0).y > 30.0f);
}

static void test_layout_motion() {
    // Enter at the end of line 0: "b" is pushed down smoothly, not at once.
    Editor ed = fresh("a\nb\nc");
    float b0 = ed.drawn_y(1);
    key(ed, ImGuiKey_End);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    frame(ed);  // the edit happens here; heights start animating
    io.AddKeyEvent(ImGuiKey_Enter, false);
    float laid = ed.laid_y(2);           // "b" is now line 2
    float mid = ed.drawn_y(2);
    CHECK(laid > b0);
    CHECK(mid < laid - 1.0f);            // still on its way
    CHECK(mid >= b0 - 0.5f);             // and never jumped backwards
    for (int i = 0; i < 60; i++) frame(ed);
    CHECK(std::fabs(ed.drawn_y(2) - ed.laid_y(2)) < 0.5f);  // settles exactly

    // Delete a line: the space collapses and "c" rises smoothly.
    Editor d = fresh("a\nb\nc");
    key(d, ImGuiKey_DownArrow);
    float c0 = d.drawn_y(2);
    d.delete_lines();
    frame(d);
    float now = d.drawn_y(1);            // "c" is now line 1
    CHECK(now > d.laid_y(1) + 1.0f);     // hasn't snapped up
    CHECK(now <= c0 + 0.5f);
    CHECK(std::fabs(now - c0) < 0.5f);   // the removed line fades first; "c" waits
    for (int i = 0; i < 8; i++) frame(d);
    CHECK(d.drawn_y(1) < c0 - 0.5f);     // then its space closes and "c" rises
    for (int i = 0; i < 60; i++) frame(d);
    CHECK(std::fabs(d.drawn_y(1) - d.laid_y(1)) < 0.5f);

    // Lines are always stacked: nothing ever overlaps mid-animation.
    Editor s = fresh("one\ntwo\nthree\nfour");
    s.select_range({1, 0}, {2, 5});
    frame(s);
    s.cut();
    bool stacked = true;
    for (int f = 0; f < 20; f++) {
        frame(s);
        for (int i = 1; i < (int)s.lines().size(); i++)
            if (s.drawn_y(i) < s.drawn_y(i - 1) - 0.01f) stacked = false;
    }
    CHECK(stacked);

    // Breaking a wrapped paragraph in half: the second half must never move
    // up (it starts where its text already was); only what's below pushes
    // down, by the space the break really added.
    {
        std::string para;
        for (int i = 0; i < 30; i++) para += "asdasd asda ";
        Editor b = fresh(para + "\nbelow");
        float below0 = b.drawn_y(1);
        b.select_range({0, (int)para.size() / 2}, {0, (int)para.size() / 2});
        frame(b);
        ImGuiIO& bio = ImGui::GetIO();
        bio.AddKeyEvent(ImGuiKey_Enter, true);
        frame(b);
        bio.AddKeyEvent(ImGuiKey_Enter, false);
        float half0 = b.drawn_y(1);           // the second half, first frame
        float below_first = b.drawn_y(2);
        bool never_up = true;
        float prev_half = half0, prev_below = below_first;
        for (int i = 0; i < 40; i++) {
            frame(b);
            if (b.drawn_y(1) < prev_half - 0.5f || b.drawn_y(2) < prev_below - 0.5f) never_up = false;
            prev_half = b.drawn_y(1);
            prev_below = b.drawn_y(2);
        }
        CHECK(never_up);
        CHECK(std::fabs(half0 - b.laid_y(1)) < 0.5f);  // starts in place
        CHECK(below_first >= below0 - 0.5f);           // what's below never jumps up first
        CHECK(std::fabs(b.drawn_y(2) - b.laid_y(2)) < 0.5f);
    }

    // Joining two wrapped paragraphs: what's below only ever moves up, and
    // settles in place.
    {
        std::string para;
        for (int i = 0; i < 20; i++) para += "asdasd asda ";
        Editor j = fresh(para + "\n" + para + "\nbelow");
        j.select_range({1, 0}, {1, 0});
        frame(j);
        float prev = j.drawn_y(2);
        ImGuiIO& jio = ImGui::GetIO();
        jio.AddKeyEvent(ImGuiKey_Backspace, true);
        frame(j);
        jio.AddKeyEvent(ImGuiKey_Backspace, false);
        bool never_down = j.drawn_y(1) <= prev + 0.5f;  // "below" is line 1 now
        prev = j.drawn_y(1);
        for (int i = 0; i < 40; i++) {
            frame(j);
            if (j.drawn_y(1) > prev + 0.5f) never_down = false;
            prev = j.drawn_y(1);
        }
        CHECK(never_down);
        CHECK(std::fabs(j.drawn_y(1) - j.laid_y(1)) < 0.5f);
    }

    // After an idle pause the first frame has a huge delta time. The push
    // must still animate instead of finishing in that one frame.
    {
        Editor p = fresh("a\nb");
        key(p, ImGuiKey_End);
        float b_before = p.drawn_y(1);
        ImGuiIO& pio = ImGui::GetIO();
        pio.AddKeyEvent(ImGuiKey_Enter, true);
        g_dt_override = 0.8f;  // as if delta had been asleep for 0.8s
        frame(p);
        g_dt_override = 0.0f;
        pio.AddKeyEvent(ImGuiKey_Enter, false);
        CHECK(p.drawn_y(2) < p.laid_y(2) - 1.0f);
        CHECK(p.drawn_y(2) >= b_before - 0.5f);
    }

    // A new document appears as it is.
    Editor n = fresh("x");
    n.set_text("p\nq\nr");
    frame(n);
    CHECK(std::fabs(n.drawn_y(2) - n.laid_y(2)) < 0.5f);
}

// The code-block background drawn in the last frame, as vertical spans.
// Finds its rectangles by a marker colour set in the test.
static std::vector<std::pair<float, float>> code_bg_spans(ImU32 marker) {
    std::vector<std::pair<float, float>> spans;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int n = 0; dd && n < dd->CmdLists.Size; n++) {
        const ImDrawList* dl = dd->CmdLists[n];
        for (int v = 0; v + 3 < dl->VtxBuffer.Size; v++) {
            if (dl->VtxBuffer[v].col != marker) continue;
            float lo = dl->VtxBuffer[v].pos.y, hi = lo;
            for (int k = 1; k < 4; k++) {
                lo = std::min(lo, dl->VtxBuffer[v + k].pos.y);
                hi = std::max(hi, dl->VtxBuffer[v + k].pos.y);
            }
            spans.push_back({lo, hi});
            v += 3;
        }
    }
    std::sort(spans.begin(), spans.end());
    return spans;
}

static bool no_gaps(std::vector<std::pair<float, float>> spans) {
    if (spans.empty()) return false;
    float reach = spans[0].second;
    for (auto& s : spans) {
        if (s.first > reach + 1.0f) return false;
        reach = std::max(reach, s.second);
    }
    return true;
}

static void test_code_block_space() {
    Theme saved = T;
    const ImU32 marker = IM_COL32(1, 2, 3, 255);
    T.code_bg = marker;
    T.text = T.bright = T.dim = T.faint = T.code = IM_COL32(200, 200, 200, 255);

    // Open lines inside a block, one per frame: the background stays whole.
    Editor e = fresh("```\nfloat Test = 10.0;\n```\nafter");
    e.select_range({1, 5}, {1, 5});
    frame(e);
    bool whole = true;
    ImGuiIO& io = ImGui::GetIO();
    for (int n = 0; n < 4; n++) {
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame(e);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        for (int f = 0; f < 3; f++) {
            if (!no_gaps(code_bg_spans(marker))) whole = false;
            frame(e);
        }
    }
    CHECK(whole);

    // And while they close again.
    whole = true;
    for (int n = 0; n < 4; n++) {
        io.AddKeyEvent(ImGuiKey_Backspace, true);
        frame(e);
        io.AddKeyEvent(ImGuiKey_Backspace, false);
        for (int f = 0; f < 3; f++) {
            if (!no_gaps(code_bg_spans(marker))) whole = false;
            frame(e);
        }
    }
    CHECK(whole);
    e.select_range({2, 0}, {2, 0});
    frame(e);
    e.delete_lines();
    whole = true;
    for (int f = 0; f < 20; f++) {
        frame(e);
        if (!no_gaps(code_bg_spans(marker))) whole = false;
    }
    CHECK(whole);
    T = saved;
}

static void test_change_pulse() {
    TextPos a, b;
    auto is = [](TextPos p, int line, int col) { return p.line == line && p.col == col; };

    // Paste: exactly the pasted text.
    Editor p = fresh("abcd");
    ImGui::SetClipboardText("XYZ");
    p.select_range({0, 2}, {0, 2});
    frame(p);
    p.paste();
    frame(p);
    CHECK(p.pulse_range(a, b) && is(a, 0, 2) && is(b, 0, 5));

    // Multi-line paste spans the lines it added.
    Editor m = fresh("start end");
    ImGui::SetClipboardText("one\ntwo\n");
    m.select_range({0, 6}, {0, 6});
    frame(m);
    m.paste();
    frame(m);
    CHECK(m.pulse_range(a, b) && is(a, 0, 6) && is(b, 2, 0));

    // Undo that only removes text: nothing to highlight. Redo brings it back
    // and highlights it.
    Editor u = fresh("");
    type(u, "hello");
    u.undo();
    frame(u);
    CHECK(!u.pulse_range(a, b));
    u.redo();
    frame(u);
    CHECK(u.pulse_range(a, b) && is(a, 0, 0) && is(b, 0, 5));

    // Undoing a line delete highlights the restored line.
    Editor d = fresh("keep\ngone\nkeep");
    d.select_range({1, 0}, {1, 0});
    frame(d);
    d.delete_lines();
    frame(d);
    d.undo();
    frame(d);
    CHECK(d.pulse_range(a, b) && a.line == 1 && b.line <= 2);

    // Any later edit clears it (its positions would be stale).
    type(u, "!");
    CHECK(!u.pulse_range(a, b));
}

static void test_morph() {
    // Every kind of line, both directions, every frame of the transition:
    // nothing out of range, and the text is untouched.
    std::string long_item = "- a list item long enough to wrap onto another row, with **bold** and `code` and a [link](https://x.y) in it";
    std::string doc = "# Title\n\nPara with *em* and ==mark== and [[wiki|alias]].\n\n" + long_item +
                      "\n- [ ] task\n- [x] done\n1. first\n> quote line\n---\n```cpp\nint x = 1;\n```\n\n~~gone~~ end";
    Editor e = fresh(doc);
    for (int round = 0; round < 2; round++) {
        e.set_reader(!e.reader());
        for (int f = 0; f < 20; f++) frame(e);
    }
    CHECK(!e.reader());
    CHECK_EQ(e.text(), doc);

    // Toggling again mid-morph is fine too.
    e.set_reader(true);
    frame(e);
    frame(e);
    e.set_reader(false);
    for (int f = 0; f < 20; f++) frame(e);
    CHECK_EQ(e.text(), doc);
}

// Glyph corner positions of all text drawn in `col` in the last frame.
static std::vector<std::pair<float, float>> glyphs_in(ImU32 col) {
    std::vector<std::pair<float, float>> out;
    ImDrawData* dd = ImGui::GetDrawData();
    for (int n = 0; dd && n < dd->CmdLists.Size; n++)
        for (auto& v : dd->CmdLists[n]->VtxBuffer)
            if (v.col == col) out.push_back({v.pos.x, v.pos.y});
    std::sort(out.begin(), out.end());
    return out;
}

// Same glyphs in the same places, to within float rounding (0.05px).
static bool same_glyphs(const std::vector<std::pair<float, float>>& a, const std::vector<std::pair<float, float>>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::fabs(a[i].first - b[i].first) > 0.05f || std::fabs(a[i].second - b[i].second) > 0.05f) return false;
    return true;
}

static void test_morph_still_text() {
    // Plain text has nothing to move between the views: every glyph must sit
    // on exactly the same spot in the editor, all through the morph, and in
    // the reader. (It used to shiver by half a pixel.)
    Theme saved = T;
    const ImU32 ink = IM_COL32(210, 211, 212, 255);
    T.text = T.bright = ink;
    T.dim = T.faint = IM_COL32(90, 90, 90, 255);
    std::string doc = "Plain words on a line, long enough that it wraps onto a second row in the column for sure, twice even, "
                      "because the column is only so wide and this keeps going.\n\nAnother plain line.";
    Editor e = fresh(doc);
    for (int f = 0; f < 5; f++) frame(e);
    auto still = glyphs_in(ink);
    CHECK(!still.empty());
    bool same = true;
    e.set_reader(true);
    for (int f = 0; f < 30; f++) {
        frame(e);
        if (!same_glyphs(glyphs_in(ink), still)) same = false;
    }
    CHECK(same);
    same = true;
    e.set_reader(false);
    for (int f = 0; f < 30; f++) {
        frame(e);
        if (!same_glyphs(glyphs_in(ink), still)) same = false;
    }
    CHECK(same);
    T = saved;
}

static void test_find_replace() {
    Editor e = fresh("foo bar Foo\nfoo end");
    e.find_begin();
    e.find_set("foo");  // smart case: lowercase matches any case
    CHECK_EQ(e.find_count(), 3);
    CHECK_EQ(e.selected_text(), std::string("foo"));
    CHECK(e.anchor().line == 0 && e.anchor().col == 0);
    e.find_step(1);
    CHECK(e.anchor().col == 8);          // "Foo"
    e.find_step(1);
    CHECK(e.anchor().line == 1);
    e.find_step(1);
    CHECK(e.anchor().line == 0 && e.anchor().col == 0);  // wraps
    e.find_step(-1);
    CHECK(e.anchor().line == 1);         // and back
    e.find_set("Foo");                   // a capital: case-sensitive
    CHECK_EQ(e.find_count(), 1);
    e.find_set("zzz");
    CHECK_EQ(e.find_count(), 0);
    CHECK(!e.has_selection());

    // Replace one: moves on to the next match.
    Editor r = fresh("a cat, a cat, a cat");
    r.find_begin();
    r.find_set("cat");
    r.replace_current("dog");
    frame(r);
    CHECK_EQ(r.text(), std::string("a dog, a cat, a cat"));
    CHECK_EQ(r.find_count(), 2);
    CHECK(r.anchor().col == 9);
    // Replace all: one undo step, each replacement pulses.
    CHECK_EQ(r.replace_all("bird"), 2);
    frame(r);
    CHECK_EQ(r.text(), std::string("a dog, a bird, a bird"));
    CHECK_EQ(r.pulse_spans(), 2);
    r.undo();
    frame(r);
    CHECK_EQ(r.text(), std::string("a dog, a cat, a cat"));
    // A replacement containing the query isn't matched again.
    Editor g = fresh("x x");
    g.find_begin();
    g.find_set("x");
    CHECK_EQ(g.replace_all("xx"), 2);
    CHECK_EQ(g.text(), std::string("xx xx"));
    g.find_end();
}

static void test_navigation_pass() {
    // Headings.
    Editor h = fresh("# One\ntext\n## Two\nmore\n# Three");
    h.goto_heading(1);
    CHECK(h.cursor().line == 2 && h.cursor().col == 3);
    h.goto_heading(1);
    CHECK(h.cursor().line == 4);
    h.goto_heading(-1);
    CHECK(h.cursor().line == 2);

    // Paragraphs: ctrl+down to the next one's start, ctrl+up back.
    Editor p = fresh("a1\na2\n\nb1\nb2\n\n\nc1");
    key(p, ImGuiKey_DownArrow, ImGuiMod_Ctrl);
    CHECK(p.cursor().line == 3 && p.cursor().col == 0);
    key(p, ImGuiKey_DownArrow, ImGuiMod_Ctrl);
    CHECK(p.cursor().line == 7);
    key(p, ImGuiKey_UpArrow, ImGuiMod_Ctrl);
    CHECK(p.cursor().line == 3);
    key(p, ImGuiKey_RightArrow);
    key(p, ImGuiKey_UpArrow, ImGuiMod_Ctrl);
    CHECK(p.cursor().line == 3 && p.cursor().col == 0);  // mid-line: start of this paragraph

    // Expand: word, sentence, line text, whole line, paragraph, section, note.
    Editor x = fresh("# Head\n- First one. Second part here.\nnext line\n\n# Other");
    x.select_range({1, 21}, {1, 21});  // in "part"
    frame(x);
    x.expand_selection();
    CHECK_EQ(x.selected_text(), std::string("part"));
    x.expand_selection();
    CHECK_EQ(x.selected_text(), std::string("Second part here."));
    x.expand_selection();
    CHECK_EQ(x.selected_text(), std::string("First one. Second part here."));
    x.expand_selection();
    CHECK_EQ(x.selected_text(), std::string("- First one. Second part here."));
    x.expand_selection();
    CHECK_EQ(x.selected_text(), std::string("# Head\n- First one. Second part here.\nnext line"));
    x.expand_selection();  // section is the same span here, so the whole note is next
    CHECK_EQ(x.selected_text(), x.text());
    x.shrink_selection();
    x.shrink_selection();
    CHECK_EQ(x.selected_text(), std::string("- First one. Second part here."));

    // Link at the caret.
    Editor l = fresh("see [[My Note]] and [site](https://a.b)");
    LinkSpan link;
    l.select_range({0, 7}, {0, 7});
    CHECK(l.link_at_caret(link) && link.wiki && link.target == "My Note");
    l.select_range({0, 22}, {0, 22});
    CHECK(l.link_at_caret(link) && !link.wiki && link.target == "https://a.b");
    l.select_range({0, 1}, {0, 1});
    CHECK(!l.link_at_caret(link));
}

static void test_writing_pass() {
    // A URL pasted over selected text becomes a link.
    Editor u = fresh("read the docs today");
    ImGui::SetClipboardText("https://example.com/docs");
    u.select_range({0, 9}, {0, 13});
    frame(u);
    u.paste();
    CHECK_EQ(u.text(), std::string("read the [docs](https://example.com/docs) today"));
    // Not over a selection: pasted as it is.
    u.select_range({0, 0}, {0, 0});
    frame(u);
    u.paste();
    CHECK(u.text().rfind("https://example.com/docs", 0) == 0);

    // Ordered lists renumber.
    Editor d = fresh("1. a\n2. b\n3. c\n4. d");
    d.select_range({1, 0}, {1, 0});
    frame(d);
    d.delete_lines();
    CHECK_EQ(d.text(), std::string("1. a\n2. c\n3. d"));
    d.undo();
    frame(d);
    CHECK_EQ(d.text(), std::string("1. a\n2. b\n3. c\n4. d"));

    Editor n = fresh("1. a\n2. b");
    n.select_range({0, 4}, {0, 4});
    frame(n);
    key(n, ImGuiKey_Enter);
    type(n, "new");
    CHECK_EQ(n.text(), std::string("1. a\n2. new\n3. b"));

    Editor m = fresh("1. a\n2. b\n3. c");
    m.select_range({2, 0}, {2, 0});
    frame(m);
    m.move_lines(-1);
    CHECK_EQ(m.text(), std::string("1. a\n2. c\n3. b"));

    // Nested lists count on their own; start number is kept.
    Editor s = fresh("5. a\n    1. x\n    2. y\n6. b\n7. c");
    s.select_range({3, 0}, {3, 0});
    frame(s);
    s.delete_lines();
    CHECK_EQ(s.text(), std::string("5. a\n    1. x\n    2. y\n6. c"));

    // Numbers growing a digit keep the caret in place.
    std::string many;
    for (int i = 1; i <= 9; i++) many += std::to_string(i) + ". i\n";
    Editor w = fresh(many + "9. last");
    w.select_range({9, 7}, {9, 7});
    frame(w);
    CHECK_EQ(w.lines()[9], std::string("9. last"));
    w.duplicate_lines(-1);  // renumbers the list: the last item becomes 11
    CHECK_EQ(w.lines()[10], std::string("11. last"));
}

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // Keep the test away from the real system clipboard.
    static std::string clip;
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext*, const char* t) { clip = t; };
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext*) { return clip.c_str(); };
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    g_sans = io.Fonts->AddFontFromMemoryTTF((void*)g_font_sans, (int)g_font_sans_size, 17, &fc);
    g_mono = io.Fonts->AddFontFromMemoryTTF((void*)g_font_mono, (int)g_font_mono_size, 17, &fc);

    test_markdown();
    test_typing();
    test_lists();
    test_lines();
    test_wrap_and_clipboard();
    test_vertical_and_wrap();
    test_reader();
    test_fuzzy_and_keys();
    test_ini();
    test_config_schema();
    test_images();
    test_smear();
    test_layout_motion();
    test_code_block_space();
    test_change_pulse();
    test_morph();
    test_morph_still_text();
    test_find_replace();
    test_navigation_pass();
    test_writing_pass();

    ImGui::DestroyContext();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
