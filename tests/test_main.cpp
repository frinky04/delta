// Headless tests: drives the real Editor through ImGui frames with simulated
// input. No window or GPU; font textures are requested but never uploaded.
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
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
    int score;
    std::vector<int> pos;
    CHECK(fuzzy_match("drm", "delta roadmap", score, &pos));
    CHECK(!fuzzy_match("xyz", "delta roadmap", score, nullptr));
    int s1, s2;
    fuzzy_match("road", "delta roadmap", s1, nullptr);
    fuzzy_match("road", "r o a d", s2, nullptr);
    CHECK(s1 > s2);

    Chord c;
    CHECK(parse_chord("ctrl+shift+p", c));
    CHECK_EQ(chord_str(c), std::string("ctrl+shift+p"));
    CHECK(parse_chord("ctrl+,", c));
    CHECK(c.key == ImGuiKey_Comma);
    CHECK(parse_chord("ctrl+=", c));
    CHECK(parse_chord("alt+up", c));
    CHECK(!parse_chord("hyper+q", c));
    std::vector<std::string> bad;
    auto list = parse_chords("ctrl+z  ctrl+shift+z nope+x", &bad);
    CHECK_EQ((int)list.size(), 2);
    CHECK_EQ((int)bad.size(), 1);
}

static void test_ini() {
    LineInfo info;
    std::vector<uint16_t> st;
    ini_analyze("[editor]", info, st);
    CHECK(st[0] & I_Bracket);
    CHECK(st[1] & I_Section);
    CHECK(st[0] & S_CodeBlock);  // everything is mono

    ini_analyze("font_size = 14 ; px", info, st);
    CHECK(st[0] & I_Key);
    CHECK(st[10] & I_Eq);
    CHECK(st[12] & I_Value);
    CHECK(st[15] & I_Comment);
    CHECK(!(st[14] & I_Value));  // trailing space before the comment

    ini_analyze("accent = #e6e6e6", info, st);
    CHECK(st[10] & I_Color);
    ini_analyze(";font_size = 14", info, st);
    CHECK(st[5] & I_Comment);
    CHECK(!(st[5] & I_Key));

    std::string name;
    CHECK(ini_section("  [keys]  ", name));
    CHECK_EQ(name, std::string("keys"));
    CHECK(!ini_section("key = [x]", name));

    // Problems are tied to 0-based lines.
    fs::path p = fs::temp_directory_path() / "delta_test_config.ini";
    atomic_write(p,
                 "; comment\n"
                 "[editor]\n"
                 "font_size = big\n"
                 "line_numbers = maybe\n"
                 "wat = 1\n"
                 "tab_size = 99\n"
                 "[colors]\n"
                 "accent = #123456\n"
                 "[nope]\n",
                 nullptr);
    Config cfg;
    std::vector<ConfigIssue> issues;
    CHECK(load_config(p, cfg, issues));
    fs::remove(p);
    auto at = [&](int line) {
        for (auto& i : issues)
            if (i.line == line) return i.msg;
        return std::string();
    };
    CHECK_EQ(at(2), std::string("expected a number, 8 to 48, using 14"));
    CHECK_EQ(at(3), std::string("expected true or false, using false"));
    CHECK_EQ(at(4), std::string("unknown setting"));
    CHECK(at(5).rfind("out of range", 0) == 0);
    CHECK(at(8).rfind("unknown section", 0) == 0);
    CHECK_EQ(cfg.tab_size, 8);
    CHECK_EQ(cfg.line_of("colors.accent"), 7);
}

static void test_config_schema() {
    // Every table entry has a default that passes its own check.
    for (auto& s : all_settings()) {
        CHECK(find_setting(s.section, s.key) == &s);
        CHECK(same_value(s, s.def, s.def));
    }

    CHECK_EQ(suggest("line_hieght", {"line_height", "line_numbers"}), std::string("line_height"));
    CHECK_EQ(suggest("zzzzzz", {"line_height", "theme"}), std::string());
    const Setting* fs_ = find_setting("editor", "font_size");
    CHECK(same_value(*fs_, "14.0", "14"));
    CHECK(!same_value(*fs_, "15", "14"));
    CHECK(same_value(*find_setting("editor", "line_numbers"), "no", "false"));
    CHECK(same_value(*find_setting("editor", "caret_style"), "THICK", "thick"));
    CHECK_EQ(find_setting("colors", "accent")->default_for("light"), std::string("#c96f0e"));
    CHECK_EQ(find_setting("colors", "accent")->default_for("dark"), std::string("#e0851e"));

    fs::path p = fs::temp_directory_path() / "delta_test_schema.ini";
    atomic_write(p,
                 "[editr]\n"
                 "[editor]\n"
                 "line_hieght = 2\n"
                 "theme = light\n"
                 "caret_style = round\n"
                 "[colors]\n"
                 "accent = orange\n",
                 nullptr);
    Config cfg;
    std::vector<ConfigIssue> issues;
    CHECK(load_config(p, cfg, issues));
    fs::remove(p);
    auto at = [&](int line) {
        for (auto& i : issues)
            if (i.line == line) return i.msg;
        return std::string();
    };
    CHECK_EQ(at(0), std::string("unknown section; did you mean [editor]?"));
    CHECK_EQ(at(2), std::string("unknown setting; did you mean line_height?"));
    CHECK_EQ(at(3), std::string("belongs in [appearance]"));
    CHECK_EQ(at(4), std::string("use bar, thick or block, using thick"));
    CHECK_EQ(at(6), std::string("expected a colour like #e0851e, using the default"));
    CHECK_EQ(cfg.caret_style, std::string("thick"));
    CHECK(cfg.colors.empty());

    // Each problem points at the part it's about: the section name, the key
    // for names, the value for values (inline comments and blanks left out).
    Config live;
    std::vector<ConfigIssue> li;
    parse_config("  [editr]\n[editor]\nline_hieght = 2\ncaret_style =  round   ; comment\nfont_size =\n", live, li);
    auto span = [&](int line) {
        for (auto& i : li)
            if (i.line == line) return std::make_pair(i.col0, i.col1);
        return std::make_pair(-2, -2);
    };
    CHECK(span(0) == std::make_pair(3, 8));    // editr
    CHECK(span(2) == std::make_pair(0, 11));   // line_hieght
    CHECK(span(3) == std::make_pair(15, 20));  // round
    CHECK(span(4) == std::make_pair(0, 9));    // empty value: the key
    CHECK_EQ(live.where["editor.caret_style"].val0, 15);
    ConfigIssue ia = issue_at(live, "editor.caret_style", true, "x");
    CHECK(ia.line == 3 && ia.col0 == 15 && ia.col1 == 20);
    CHECK_EQ(issue_at(live, "nope.nope", true, "x").line, -1);

    // A fresh config is just the sections.
    std::string tpl = default_config_text();
    for (const char* s : kSections) CHECK(tpl.find(std::string("[") + s + "]") != std::string::npos);
    CHECK(tpl.find('=') == std::string::npos);

    // Hints: what each setting does and its default, for this theme.
    std::vector<std::string> lines = {
        "; hi",
        "[editor]",
        "font_size = 14",
        "font_size = 16",
        "[colors]",
        "accent = #c96f0e",
        "[keys]",
        "edit.undo = ctrl+z",
        "edit.redo = ctrl+shift+z ctrl+y",
        "note.move = ctrl+m",
        "edit.cut = ctrl+k",
    };
    std::vector<BindingDoc> docs = {{"edit.undo", "Undo", "ctrl+z"}, {"edit.redo", "Redo", "ctrl+y ctrl+shift+z"},
                                    {"note.move", "Move note", ""}, {"edit.cut", "Cut", "ctrl+x"}};
    auto hints = config_hints(lines, "light", docs);
    CHECK(hints.count(0) == 0 && hints.count(1) == 0);
    CHECK_EQ(hints[2], std::string("px (default)"));
    CHECK_EQ(hints[3], std::string("px, default 14"));
    CHECK(hints[5].find("(default)") != std::string::npos);  // light theme's accent
    CHECK_EQ(hints[7], std::string("(default)"));
    CHECK_EQ(hints[8], std::string("(default)"));    // same chords, any order
    CHECK_EQ(hints[9], std::string("unbound by default"));
    CHECK_EQ(hints[10], std::string("default ctrl+x"));
    CHECK(config_hints(lines, "dark", docs)[5].find("default #e0851e") != std::string::npos);
    // Self-explanatory settings show only their default.
    CHECK_EQ(config_hints({"[editor]", "line_numbers = true"}, "dark", {})[1], std::string("default false"));

    // New settings go after the last one in their section, or in a new
    // section at the end.
    bool header = false;
    std::vector<std::string> doc = {"[general]", "", "[editor]", "font_size = 15", "; tabs", "", "[keys]", ""};
    CHECK_EQ(config_insert_line(doc, "general", header), 1);
    CHECK(!header);
    CHECK_EQ(config_insert_line(doc, "editor", header), 4);
    CHECK(!header);
    CHECK_EQ(config_insert_line(doc, "keys", header), 7);
    CHECK_EQ(config_insert_line(doc, "colors", header), 8);
    CHECK(header);
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
