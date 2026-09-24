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

    // Block kinds the editor and reader branch on.
    md_analyze("## Title", false, info, st);
    CHECK(info.kind == LineKind::Heading && info.level == 2);
    md_analyze("#tag", false, info, st);
    CHECK(info.kind != LineKind::Heading);  // a tag, not a heading
    md_analyze("---", false, info, st);
    CHECK(info.kind == LineKind::Rule);
    md_analyze("- a", false, info, st);
    CHECK(info.list && !info.ordered && info.kind != LineKind::Rule);
    md_analyze("3. third", false, info, st);
    CHECK(info.list && info.ordered);
    md_analyze("  - [x] done", false, info, st);
    CHECK(info.list && info.task && info.done);
    md_analyze("```js", false, info, st);
    CHECK(info.kind == LineKind::Fence);
    md_analyze("# **not** heading", true, info, st);  // inside a fence: verbatim
    CHECK(info.kind == LineKind::CodeBlock && !(st[4] & S_Bold));

    // Inline emphasis covers its text and nothing outside it.
    md_analyze("a **bold** b", false, info, st);
    CHECK((st[4] & S_Bold) && !(st[0] & S_Bold) && !(st[11] & S_Bold));
    md_analyze("x *it* y", false, info, st);
    CHECK((st[3] & S_Italic) && !(st[7] & S_Italic));
    md_analyze("snake_case_name here", false, info, st);
    CHECK(!(st[6] & S_Italic));  // intraword underscores are not emphasis
    md_analyze("`**not bold**`", false, info, st);
    CHECK((st[3] & S_Code) && !(st[3] & S_Bold));

    // Links are what clicking follows.
    std::vector<LinkSpan> links;
    md_analyze("see [[Other note|alias]] and [x](https://a.b)", false, info, st, &links);
    CHECK_EQ((int)links.size(), 2);
    if (links.size() == 2) {
        CHECK(links[0].wiki && links[0].target == "Other note");
        CHECK(!links[1].wiki && links[1].target == "https://a.b");
    }

    // Reader text drops the syntax, and its map always points back into the
    // source in order (clicks and selections in the reader rely on it).
    std::string out;
    std::vector<uint16_t> ost;
    std::vector<int> map;
    md_analyze("## Hello **world**", false, info, st);
    md_reader("## Hello **world**", info, st, out, ost, map);
    CHECK_EQ(out, std::string("Hello world"));
    bool ok = true;
    for (const char* s : {"> - [ ] a *b* [c](d) ==e== ~~f~~", "1. **x** `y` [[z|w]]", "- item", "plain"}) {
        md_analyze(s, false, info, st);
        md_reader(s, info, st, out, ost, map);
        ok = ok && !out.empty() && map.size() == out.size() && ost.size() == out.size();
        for (size_t i = 0; ok && i < map.size(); i++)
            ok = map[i] >= 0 && map[i] < (int)std::string(s).size() && (i == 0 || map[i] >= map[i - 1]);
    }
    CHECK(ok);
}

static void test_typing() {
    Editor ed = fresh("");
    type(ed, "hello");
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("hell"));
    key(ed, ImGuiKey_Backspace, ImGuiMod_Ctrl);
    CHECK_EQ(ed.text(), std::string(""));
    type(ed, "one two");
    key(ed, ImGuiKey_LeftArrow, ImGuiMod_Ctrl);
    type(ed, "X");
    CHECK_EQ(ed.text(), std::string("one Xtwo"));
    // Undo takes back the edit at the other spot, then the typing run as
    // one step.
    ed.undo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("one two"));
    ed.undo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string(""));
    // A new edit after undo drops the redo branch.
    type(ed, "Y");
    ed.redo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("Y"));

    // Separate edits undo one at a time, all the way back, then redo forward.
    Editor h = fresh("abc");
    key(h, ImGuiKey_End);
    key(h, ImGuiKey_Enter);
    type(h, "def");
    h.select_range({0, 0}, {1, 1});
    frame(h);
    key(h, ImGuiKey_Delete);
    CHECK_EQ(h.text(), std::string("ef"));
    h.undo();
    frame(h);
    CHECK_EQ(h.text(), std::string("abc\ndef"));
    for (int i = 0; i < 10; i++) { h.undo(); frame(h); }
    CHECK_EQ(h.text(), std::string("abc"));
    for (int i = 0; i < 10; i++) { h.redo(); frame(h); }
    CHECK_EQ(h.text(), std::string("ef"));

    // Enter splits a line; backspace at a line start and delete at a line
    // end join it back.
    Editor s = fresh("helloworld");
    s.select_range({0, 5}, {0, 5});
    frame(s);
    key(s, ImGuiKey_Enter);
    CHECK_EQ(s.text(), std::string("hello\nworld"));
    key(s, ImGuiKey_Backspace);
    CHECK_EQ(s.text(), std::string("helloworld"));
    key(s, ImGuiKey_Enter);
    key(s, ImGuiKey_LeftArrow);
    key(s, ImGuiKey_Delete);
    CHECK_EQ(s.text(), std::string("helloworld"));

    // Typing replaces a selection, across lines too.
    Editor r = fresh("one\ntwo\nthree");
    r.select_range({0, 1}, {2, 2});
    frame(r);
    type(r, "X");
    CHECK_EQ(r.text(), std::string("oXree"));

    // Multi-byte characters are stepped and deleted whole.
    Editor u = fresh("a\xC3\xA9");
    key(u, ImGuiKey_End);
    key(u, ImGuiKey_Backspace);
    CHECK_EQ(u.text(), std::string("a"));
    Editor u2 = fresh("\xE2\x82\xAC\xC3\xA9");  // euro sign, e-acute
    key(u2, ImGuiKey_End);
    key(u2, ImGuiKey_LeftArrow);
    type(u2, "x");
    key(u2, ImGuiKey_Home);
    key(u2, ImGuiKey_Delete);
    CHECK_EQ(u2.text(), std::string("x\xC3\xA9"));
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

    // Nested items continue at their depth.
    Editor n = fresh("- a\n    - b");
    key(n, ImGuiKey_End, ImGuiMod_Ctrl);
    key(n, ImGuiKey_Enter);
    CHECK_EQ(n.text(), std::string("- a\n    - b\n    - "));

    // Inside a code block a "- " line is code: Enter adds no bullet.
    Editor c = fresh("```\n- x\n```");
    c.select_range({1, 3}, {1, 3});
    frame(c);
    key(c, ImGuiKey_Enter);
    CHECK_EQ(c.text(), std::string("```\n- x\n\n```"));

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
    ed.delete_lines();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("a\nc\nb"));
    ed.undo();
    frame(ed);
    CHECK_EQ(ed.text(), std::string("a\nc\nb\nb"));
    // Moving past either end changes nothing (and loses nothing).
    Editor f = fresh("x\ny");
    f.move_lines(-1);
    key(f, ImGuiKey_DownArrow);
    f.move_lines(1);
    CHECK_EQ(f.text(), std::string("x\ny"));
    // A multi-line selection moves as a block.
    Editor b = fresh("1\n2\n3\n4");
    b.select_range({1, 0}, {2, 1});
    frame(b);
    b.move_lines(1);
    frame(b);
    CHECK_EQ(b.text(), std::string("1\n4\n2\n3"));
}

static void test_wrap_and_clipboard() {
    Editor ed = fresh("make this bold");
    ed.select_range({0, 5}, {0, 9});
    frame(ed);
    ed.wrap_selection("**");
    frame(ed);
    CHECK_EQ(ed.text(), std::string("make **this** bold"));
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

    // Cut then paste puts back exactly what was there, across lines.
    const std::string doc = "alpha beta\ngamma\n  delta \xC3\xA9psilon\n";
    Editor x = fresh(doc);
    x.select_range({0, 6}, {2, 2});
    frame(x);
    x.cut();
    frame(x);
    CHECK_EQ(x.text(), std::string("alpha delta \xC3\xA9psilon\n"));
    x.paste();
    frame(x);
    CHECK_EQ(x.text(), doc);
    // Pasting over a selection replaces it; CRLF from other apps comes in as
    // plain line breaks. Undo takes the paste back.
    ImGui::SetClipboardText("one\r\ntwo");
    x.select_range({0, 0}, {0, 5});
    frame(x);
    x.paste();
    frame(x);
    CHECK_EQ(x.text(), std::string("one\ntwo beta\ngamma\n  delta \xC3\xA9psilon\n"));
    x.undo();
    frame(x);
    CHECK_EQ(x.text(), doc);

    // Cutting a whole line (no selection) and pasting it elsewhere moves it.
    Editor l = fresh("a\nb\nc");
    l.cut();
    frame(l);
    CHECK_EQ(l.text(), std::string("b\nc"));
    key(l, ImGuiKey_DownArrow);
    l.paste();
    frame(l);
    CHECK_EQ(l.text(), std::string("b\na\nc"));
}

static void test_vertical_and_wrap() {
    // A long line wraps; down arrow moves by visual row, not by line.
    std::string long_line;
    for (int i = 0; i < 40; i++) long_line += "word ";
    Editor ed = fresh(long_line + "\nnext");
    key(ed, ImGuiKey_DownArrow);
    CHECK(ed.cursor().line == 0 && ed.cursor().col > 0);
    key(ed, ImGuiKey_End);
    key(ed, ImGuiKey_End);
    CHECK_EQ(ed.cursor().col, (int)long_line.size());
    key(ed, ImGuiKey_End, ImGuiMod_Ctrl);
    CHECK(ed.cursor() == (TextPos{1, 4}));
    key(ed, ImGuiKey_Home, ImGuiMod_Ctrl);
    CHECK(ed.cursor() == (TextPos{0, 0}));

    // Up/down keep the column across a shorter line.
    Editor v = fresh("abcdef\nab\nabcdef");
    v.select_range({0, 5}, {0, 5});
    frame(v);
    key(v, ImGuiKey_DownArrow);
    CHECK(v.cursor() == (TextPos{1, 2}));
    key(v, ImGuiKey_DownArrow);
    CHECK(v.cursor() == (TextPos{2, 5}));

    // Shift extends a selection from where it started.
    Editor s = fresh("hello world\nnext");
    key(s, ImGuiKey_RightArrow, ImGuiMod_Ctrl | ImGuiMod_Shift);
    CHECK_EQ(s.selected_text(), std::string("hello"));
    key(s, ImGuiKey_DownArrow, ImGuiMod_Shift);
    CHECK(s.anchor() == (TextPos{0, 0}) && s.cursor().line == 1);
}

static void test_reader() {
    const std::string doc = "# T\n\n- [ ] task\n\n```\ncode\n```";
    Editor ed = fresh(doc);
    ed.set_reader(true);
    frame(ed);
    type(ed, "zzz");  // reader is read-only
    key(ed, ImGuiKey_Enter);
    key(ed, ImGuiKey_Backspace);
    key(ed, ImGuiKey_Delete);
    CHECK_EQ(ed.text(), doc);
    // Back in the editor, editing works again.
    ed.set_reader(false);
    frame(ed);
    key(ed, ImGuiKey_End);
    type(ed, "!");
    CHECK_EQ(ed.text(), std::string("# T!") + doc.substr(3));
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

// Every image src in a document, sorted: an edit or move must neither lose
// nor duplicate one.
static std::vector<std::string> image_srcs(const std::string& text) {
    std::vector<std::string> out;
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::vector<ImageRef> refs;
        if (md_image_row(text.substr(at, nl - at), &refs))
            for (auto& r : refs) out.push_back(r.src);
        at = nl + 1;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Every line holding an image is still a whole image line: no token cut in
// half, none run into text or into its neighbour.
static bool image_lines_valid(const std::string& text) {
    for (size_t at = 0; at <= text.size();) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        if (line.find("![") != std::string::npos && !md_image_row(line)) return false;
        at = nl + 1;
    }
    return true;
}

static ImVec2 mid(ImVec2 a, ImVec2 b) { return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); }

// Press at `from`, drag to `to` and let go (or press escape first), then let
// any landing settle and move the pointer away.
static void drag(Editor& ed, ImVec2 from, ImVec2 to, bool escape = false) {
    ImGuiIO& io = ImGui::GetIO();
    auto at = [&](ImVec2 p, bool down) {
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(0, down);
        frame(ed);
    };
    at(from, false);
    at(from, false);
    at(from, true);
    for (int k = 1; k <= 6; k++) at(ImVec2(from.x + (to.x - from.x) * k / 6, from.y + (to.y - from.y) * k / 6), true);
    if (escape) {
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame(ed);
        io.AddKeyEvent(ImGuiKey_Escape, false);
    }
    at(to, false);
    for (int k = 0; k < 30; k++) frame(ed);
    io.AddMousePosEvent(-1, -1);
    frame(ed);
}

static void click(Editor& ed, ImVec2 p) {
    ImGuiIO& io = ImGui::GetIO();
    for (bool down : {false, true, false}) {
        io.AddMousePosEvent(p.x, p.y);
        io.AddMouseButtonEvent(0, down);
        frame(ed);
    }
}

static void test_image_markdown() {
    // What counts as an image line, and the src a note's link resolves from.
    std::string alt, src;
    CHECK(md_image_line("![a cat](cat.png)", &alt, &src) && alt == "a cat" && src == "cat.png");
    CHECK(md_image_line("  ![](../.delta/images/ab12.png)  ", nullptr, &src) && src == "../.delta/images/ab12.png");
    CHECK(md_image_line("![x](<my pics/cat 1.png> \"title\")", nullptr, &src) && src == "my pics/cat 1.png");
    CHECK(md_image_line("![[nested] alt](cat.png)", &alt, &src) && alt == "[nested] alt" && src == "cat.png");
    // Anything else stays text.
    CHECK(!md_image_line("see ![a](cat.png)"));
    CHECK(!md_image_line("![a](cat.png) more"));
    CHECK(!md_image_line("![a]()"));
    CHECK(!md_image_line("[a](cat.png)"));
    LineInfo info;
    std::vector<uint16_t> st;
    md_analyze("![](a.png)", false, info, st);
    CHECK(info.kind == LineKind::Image);
    md_analyze("![](a.png)", true, info, st);
    CHECK(info.kind == LineKind::CodeBlock);  // not inside code

    // Rows: images separated by spaces; sizes the way Obsidian writes them.
    std::vector<ImageRef> refs;
    CHECK(md_image_row("![](a.png) ![](b.png)  ![x](<c d.png>)", &refs) && refs.size() == 3 &&
          refs[0].src == "a.png" && refs[1].src == "b.png" && refs[2].src == "c d.png");
    CHECK(!md_image_row("![](a.png)![](b.png)"));
    CHECK(!md_image_row("![](a.png) and ![](b.png)"));
    CHECK(md_image_row("![cat|300](a.png)", &refs) && refs[0].alt == "cat" && refs[0].w == 300 && refs[0].h == 0);
    CHECK(md_image_row("![|300x200](a.png)", &refs) && refs[0].w == 300 && refs[0].h == 200);
    CHECK(md_image_row("![a|b](a.png)", &refs) && refs[0].alt == "a|b" && refs[0].w == 0);  // not a size

    // What the editor writes parses back to the same image; resizing keeps
    // alt, src and title and only changes the size.
    for (int w : {0, 120}) {
        std::string t = md_image_text("my cat", "my pics/cat 1.png", w);
        CHECK(md_image_row(t, &refs) && refs.size() == 1 && refs[0].alt == "my cat" &&
              refs[0].src == "my pics/cat 1.png" && refs[0].w == w);
    }
    CHECK_EQ(md_image_resized("![cat|300](a.png \"t\")", 120), std::string("![cat|120](a.png \"t\")"));
    CHECK_EQ(md_image_resized("![cat|300](a.png)", 0), std::string("![cat](a.png)"));
}

static void test_image_store() {
    CHECK_EQ(sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_hex(std::string(1000, 'a')).substr(0, 16), std::string("41edece42d63e8d9"));  // several blocks

    fs::path root = fs::temp_directory_path() / "delta_test_images";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    ImageStore store;
    store.set_root(root);
    std::string err;
    std::string link = store.add("fake png bytes", "PNG", &err);
    CHECK(err.empty());
    // A relative link into the hidden store, which from any space's folder
    // finds the stored bytes.
    CHECK(link.rfind("../.delta/images/", 0) == 0);
    std::string back;
    CHECK(read_file(resolve_image(root / "space", link), back) && back == "fake png bytes");
    // Named by content: the same bytes are one file, other bytes another.
    CHECK_EQ(store.add("fake png bytes", "png"), link);
    CHECK(store.add("other bytes", "png") != link);
    int files = 0;
    for (auto& f : fs::directory_iterator(store.dir())) { (void)f; files++; }
    CHECK_EQ(files, 2);
    fs::remove_all(root, ec);

    fs::path note_dir = root / "space";
    CHECK(resolve_image(note_dir, "https://example.com/a.png").empty());  // never fetched from the web
    CHECK(resolve_image(note_dir, "my%20cat.png") == (note_dir / "my cat.png").lexically_normal());
}

static void test_image_codecs() {
    // Clipboard formats: PNG and bitmaps round-trip exactly; formats are told apart.
    std::vector<uint8_t> px = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 255, 10, 20, 30, 0, 1, 2, 3, 255, 9, 9, 9, 9};  // 3x2
    std::string png = encode_png(px, 3, 2);
    CHECK_EQ(image_ext(png), std::string("png"));
    CHECK(image_ext("not an image").empty());
    std::vector<uint8_t> back;
    int w = 0, h = 0;
    CHECK(image_decode_bytes(png, back, w, h) && w == 3 && h == 2 && back == px);
    CHECK(dib_to_rgba(rgba_to_dib(px, 3, 2), back, w, h) && w == 3 && h == 2 && back == px);
    // A 32-bit bitmap whose fourth byte is all zero is opaque, not invisible.
    std::vector<uint8_t> clear = px;
    for (size_t i = 3; i < clear.size(); i += 4) clear[i] = 0;
    CHECK(dib_to_rgba(rgba_to_dib(clear, 3, 2), back, w, h) && back[3] == 255 && back[0] == 255);

    // Downscaling averages: a black and white checker becomes grey, and a
    // transparent pixel's colour doesn't bleed in.
    const uint8_t checker[] = {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    std::vector<uint8_t> out;
    resample_rgba(checker, 2, 2, 1, 1, out);
    CHECK(std::abs(out[0] - 128) <= 1 && out[3] == 255);
    const uint8_t half[] = {200, 0, 0, 255, 0, 255, 0, 0};
    resample_rgba(half, 2, 1, 1, 1, out);
    CHECK(out[0] == 200 && out[1] == 0 && std::abs(out[3] - 128) <= 1);
}

static bool picked(Editor& e, int line, int tile = 0) {
    int t = -1;
    return e.has_selection() && e.image_for_caret(&t) == line && t == tile;
}

// One image on a line of its own: an object the caret steps over and edits
// act on whole, never a half-deleted ![...](...).
static void test_image_editing() {
    FakeImages fake;
    g_images = &fake;
    Editor ed = fresh("para\n![](a.png)\nafter");
    ed.select_range({0, 4}, {0, 4});
    frame(ed);
    key(ed, ImGuiKey_RightArrow);
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_RightArrow);
    CHECK(!ed.has_selection() && ed.cursor().line == 2 && ed.cursor().col == 0);
    key(ed, ImGuiKey_LeftArrow);
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_LeftArrow);
    CHECK(!ed.has_selection() && ed.cursor().line == 0 && ed.cursor().col == 4);
    ed.select_range({1, 0}, {1, 0});  // a click beside it, a jump
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

    // Backspace on the empty line below takes the line and picks the image;
    // the next removes the image whole; undo brings it back.
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    CHECK(picked(ed, 1));
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n\nx\nafter"));
    ed.undo();
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    // From a line with text below it, backspace only picks it.
    ed.select_range({2, 0}, {2, 0});
    frame(ed);
    key(ed, ImGuiKey_Backspace);
    CHECK_EQ(ed.text(), std::string("para\n![](a.png)\nx\nafter"));
    CHECK(picked(ed, 1));
    // Delete on a picked image removes it whole.
    key(ed, ImGuiKey_Delete);
    CHECK_EQ(ed.text(), std::string("para\n\nx\nafter"));

    // Cut and paste carry it whole to another line.
    Editor cp = fresh("![](a.png)\ntext\n");
    cp.select_range({0, 0}, {0, 0});
    frame(cp);
    cp.cut();
    frame(cp);
    CHECK(image_srcs(cp.text()).empty());
    cp.select_range({2, 0}, {2, 0});
    frame(cp);
    cp.paste();
    frame(cp);
    CHECK_EQ(cp.text(), std::string("\ntext\n![](a.png)"));
    // Copy leaves it and pastes a second one.
    cp.select_range({2, 0}, {2, 0});
    frame(cp);
    cp.copy();
    cp.select_range({0, 0}, {0, 0});
    frame(cp);
    cp.paste();
    frame(cp);
    CHECK(image_srcs(cp.text()) == std::vector<std::string>({"a.png", "a.png"}) && image_lines_valid(cp.text()));

    // An image that can't be shown stays ordinary text.
    Editor missing = fresh("![](missing.png)");
    missing.select_range({0, 0}, {0, 0});
    frame(missing);
    key(missing, ImGuiKey_RightArrow);
    CHECK(!missing.has_selection() && missing.cursor().col == 1);

    // Pasted or dropped images arrive as whole lines: onto an empty line, or
    // splitting the caret's line; one undo takes them back.
    Editor ins = fresh("one\n\nthree");
    ins.select_range({1, 0}, {1, 0});
    frame(ins);
    ins.insert_lines({"![](a.png)", "![](b.png)"});
    CHECK_EQ(ins.text(), std::string("one\n![](a.png)\n![](b.png)\nthree"));
    ins.undo();
    CHECK_EQ(ins.text(), std::string("one\n\nthree"));
    ins.select_range({0, 2}, {0, 2});
    frame(ins);
    ins.insert_lines({"![](a.png)"});
    CHECK_EQ(ins.text(), std::string("on\n![](a.png)\ne\n\nthree"));

    // A download landing swaps its placeholder everywhere, undo history
    // too: undo and redo never bring the placeholder back.
    Editor dl = fresh("x");
    dl.select_range({0, 1}, {0, 1});
    frame(dl);
    dl.insert_lines({"![](delta-pending:1)"});
    dl.insert_lines({"![](b.png)"});  // edits after it, before the download lands
    dl.insert_lines({"![](c.png)"});
    dl.undo();  // something to redo, too
    CHECK(dl.replace_line_everywhere("![](delta-pending:1)", "![](real.png)"));
    CHECK_EQ(dl.text(), std::string("x\n![](real.png)\n![](b.png)"));
    dl.redo();
    CHECK_EQ(dl.text(), std::string("x\n![](real.png)\n![](b.png)\n![](c.png)"));
    dl.undo();
    dl.undo();
    CHECK_EQ(dl.text(), std::string("x\n![](real.png)"));
    g_images = nullptr;
}

// Rows of images on one line: each image is its own object, and moving them
// around by keyboard keeps every image and the text around them intact.
static void test_image_rows() {
    FakeImages fake;
    g_images = &fake;
    // Arrows pick image by image; backspace takes the picked one out and
    // picks the one before, delete the one that takes its place.
    Editor r = fresh("![](a.png) ![](b.png) ![](c.png)");
    r.select_range({0, 0}, {0, 0});
    frame(r);
    int t = -1;
    key(r, ImGuiKey_RightArrow);
    key(r, ImGuiKey_RightArrow);
    CHECK(r.image_for_caret(&t) == 0 && t == 2);
    key(r, ImGuiKey_LeftArrow);
    key(r, ImGuiKey_Backspace);
    CHECK_EQ(r.text(), std::string("![](a.png) ![](c.png)"));
    CHECK(r.image_for_caret(&t) == 0 && t == 0);
    // Typing with a picked image in a row goes after it, splitting the row.
    type(r, "hi");
    CHECK_EQ(r.text(), std::string("![](a.png)\nhi\n![](c.png)"));
    Editor rd = fresh("![](a.png) ![](b.png) ![](c.png)");
    rd.select_range({0, 11}, {0, 21});  // exactly b: picks it
    frame(rd);
    CHECK(picked(rd, 0, 1));
    key(rd, ImGuiKey_Delete);
    CHECK_EQ(rd.text(), std::string("![](a.png) ![](c.png)"));

    // ctrl+alt+left/right swaps the image with its neighbour in the row.
    Editor nr = fresh("![](a.png) ![](b.png) ![](c.png)");
    nr.select_range({0, 21}, {0, 21});  // just after b: on b
    frame(nr);
    key(nr, ImGuiKey_RightArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
    CHECK_EQ(nr.text(), std::string("![](a.png) ![](c.png) ![](b.png)"));
    key(nr, ImGuiKey_RightArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);  // at the end: nowhere to go
    CHECK_EQ(nr.text(), std::string("![](a.png) ![](c.png) ![](b.png)"));
    key(nr, ImGuiKey_LeftArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
    key(nr, ImGuiKey_LeftArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
    CHECK_EQ(nr.text(), std::string("![](b.png) ![](a.png) ![](c.png)"));

    // alt+up/down on a picked image: out of its row onto a line of its own,
    // into the row beside, and alone next to text, its line moves.
    Editor mv = fresh("![](a.png) ![](b.png) ![](c.png)\ntext");
    mv.select_range({0, 11}, {0, 21});  // b
    frame(mv);
    mv.move_lines(-1);
    frame(mv);
    CHECK_EQ(mv.text(), std::string("![](b.png)\n![](a.png) ![](c.png)\ntext"));
    mv.move_lines(1);  // into the row below, at its start
    frame(mv);
    CHECK_EQ(mv.text(), std::string("![](b.png) ![](a.png) ![](c.png)\ntext"));
    mv.move_lines(1);  // out below
    frame(mv);
    mv.move_lines(1);  // alone, next to text: the line moves
    frame(mv);
    CHECK_EQ(mv.text(), std::string("![](a.png) ![](c.png)\ntext\n![](b.png)"));
    CHECK(picked(mv, 2, 0));

    // Any run of moves keeps every image exactly once, every image line
    // whole and the text lines in order; undo walks all the way back and
    // redo all the way forward.
    const std::string start = "intro\n![](a.png) ![](b.png) ![](c.png)\ntext\n![](d.png) ![](e.png)\nend";
    Editor st = fresh(start);
    st.select_range({1, 11}, {1, 21});  // b
    frame(st);
    const std::vector<std::string> all = image_srcs(start);
    const int steps[] = {-1, -1, 1, 1, 1, 2, 1, 1, -2, 1, 1, -1, 2, 2, -1, -1, -1, -1, -2, 2, -2, -2, 1};  // +-1: alt, +-2: ctrl+alt
    bool kept = true, whole = true, text_ok = true, stays_picked = true;
    for (int s : steps) {
        if (s == 1 || s == -1) {
            st.move_lines(s);
            frame(st);
        } else {
            key(st, s > 0 ? ImGuiKey_RightArrow : ImGuiKey_LeftArrow, ImGuiMod_Ctrl | ImGuiMod_Alt);
        }
        std::string now = st.text();
        kept &= image_srcs(now) == all;
        whole &= image_lines_valid(now);
        size_t i = now.find("intro"), j = now.find("text"), k = now.find("end");
        text_ok &= i != std::string::npos && j != std::string::npos && k != std::string::npos && i < j && j < k;
        int line = st.image_for_caret(&t);
        stays_picked &= st.has_selection() && line >= 0 && st.image_src(line, t) == "b.png";
    }
    CHECK(kept);
    CHECK(whole);
    CHECK(text_ok);
    CHECK(stays_picked);
    std::string moved = st.text();
    CHECK(moved != start);
    for (int k = 0; k < 40; k++) st.undo();
    CHECK_EQ(st.text(), start);
    for (int k = 0; k < 40; k++) st.redo();
    CHECK_EQ(st.text(), moved);
    g_images = nullptr;
}

// The mouse on images: layout that doesn't distort or overflow, clicks that
// pick, drags that move (or cancel cleanly), and the resize handle.
static void test_image_mouse() {
    FakeImages fake;  // every image is 800x400
    g_images = &fake;
    {
        // An image keeps its shape; a row stays inside the column, one height.
        Editor s = fresh("![|300](a.png)");
        ImVec2 a, b;
        CHECK(s.image_rect(0, 0, a, b) && std::fabs((b.x - a.x) - 2 * (b.y - a.y)) < 2);
        Editor r = fresh("![](a.png) ![](b.png) ![](c.png)");
        ImVec2 a0, b0, a2, b2;
        CHECK(r.image_rect(0, 0, a0, b0) && r.image_rect(0, 2, a2, b2));
        CHECK(std::fabs((b0.y - a0.y) - (b2.y - a2.y)) < 1 && b2.x - a0.x <= 600 + 1.5f);
    }
    {
        // A click on an image picks it and changes nothing.
        Editor dk = fresh("![](a.png) ![](b.png) ![](c.png)");
        ImVec2 a, b;
        CHECK(dk.image_rect(0, 1, a, b));
        click(dk, ImVec2(a.x + 5, (a.y + b.y) * 0.5f));
        CHECK(picked(dk, 0, 1));
        CHECK_EQ(dk.text(), std::string("![](a.png) ![](b.png) ![](c.png)"));
    }
    {
        // Dragging an image onto the right half of another joins its row, after it.
        Editor dr = fresh("![](a.png)\n\n![](b.png)");
        ImVec2 sa, sb, ta, tb;
        CHECK(dr.image_rect(0, 0, sa, sb) && dr.image_rect(2, 0, ta, tb));
        drag(dr, mid(sa, sb), ImVec2(tb.x - 20, (ta.y + tb.y) * 0.5f));
        CHECK_EQ(dr.text(), std::string("\n![](b.png) ![](a.png)"));
        dr.undo();
        CHECK_EQ(dr.text(), std::string("![](a.png)\n\n![](b.png)"));
    }
    {
        // Escape mid-drag: it goes back, nothing changes.
        Editor dc = fresh("![](a.png)\n\n![](b.png)");
        ImVec2 sa, sb, ta, tb;
        CHECK(dc.image_rect(0, 0, sa, sb) && dc.image_rect(2, 0, ta, tb));
        drag(dc, mid(sa, sb), ImVec2(tb.x - 20, (ta.y + tb.y) * 0.5f), true);
        CHECK_EQ(dc.text(), std::string("![](a.png)\n\n![](b.png)"));
    }
    {
        // The handle on its right edge writes a width into the markdown; a
        // double-click takes it out again.
        Editor rs = fresh("![](a.png)\n\ntext");
        ImVec2 ia, ib;
        CHECK(rs.image_rect(0, 0, ia, ib));
        ImVec2 grip(ib.x - 1, (ia.y + ib.y) * 0.5f);
        drag(rs, grip, ImVec2(ia.x + 304, grip.y));
        std::vector<ImageRef> refs;
        std::string line0 = rs.text().substr(0, rs.text().find('\n'));
        CHECK(md_image_row(line0, &refs) && refs.size() == 1 && refs[0].src == "a.png" &&
              refs[0].w > 250 && refs[0].w < 350);
        CHECK_EQ(rs.text().substr(line0.size()), std::string("\n\ntext"));
        CHECK(rs.image_rect(0, 0, ia, ib));
        grip = ImVec2(ib.x - 1, (ia.y + ib.y) * 0.5f);
        g_time += 1.0;  // clear of the last click, for the double-click
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(grip.x, grip.y);
        frame(rs);
        for (bool down : {true, false, true, false}) {
            io.AddMousePosEvent(grip.x, grip.y);
            io.AddMouseButtonEvent(0, down);
            frame(rs);
        }
        CHECK_EQ(rs.text(), std::string("![](a.png)\n\ntext"));
        io.AddMousePosEvent(-1, -1);
        frame(rs);
    }
    g_images = nullptr;
}

static void test_images() {
    test_image_markdown();
    test_image_store();
    test_image_codecs();
    test_image_editing();
    test_image_rows();
    test_image_mouse();
}

static void test_smear() {
    // Caret/selection rect, with and without the trail: every corner stays
    // between where it was and where it's going (no overshoot, no NaN, even
    // on a huge frame gap), and it lands exactly on the target, also when
    // retargeted mid-flight.
    for (int trail = 0; trail < 2; trail++) {
        SmearRect r;
        r.snap(ImVec2(0, 0), ImVec2(4, 20));
        r.set(ImVec2(200, 300), ImVec2(204, 320), trail, 50.0f);
        bool in_range = true;
        for (int i = 0; i < 200; i++) {
            if (i == 5) r.set(ImVec2(100, 150), ImVec2(104, 170), trail, 50.0f);  // retarget mid-flight
            r.update(i == 2 ? 5.0f : 1.0f / 60);  // one long idle gap
            for (int c = 0; c < 4; c++) {
                ImVec2 p = r.corner(c);
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < -0.01f || p.x > 204.01f ||
                    p.y < -0.01f || p.y > 320.01f)
                    in_range = false;
            }
        }
        CHECK(in_range);
        CHECK(r.settled(0.001f) && r.corner(0).x == 100 && r.corner(0).y == 150 && r.corner(2).x == 104 &&
              r.corner(2).y == 170);
    }
}

// Every line's drawn position is finite and lines never overlap (each is
// drawn at or below the one above it).
static bool stacked(const Editor& ed) {
    for (int i = 0; i < (int)ed.lines().size(); i++) {
        if (!std::isfinite(ed.drawn_y(i))) return false;
        if (i > 0 && ed.drawn_y(i) < ed.drawn_y(i - 1) - 0.01f) return false;
    }
    return true;
}

// Runs frames until any motion is surely over. True if the lines stayed
// stacked on every frame and every line ends up exactly where it's laid out.
static bool settles_cleanly(Editor& ed, int frames = 90) {
    bool ok = stacked(ed);
    for (int f = 0; f < frames; f++) {
        frame(ed);
        if (!stacked(ed)) ok = false;
    }
    for (int i = 0; i < (int)ed.lines().size(); i++)
        if (std::fabs(ed.drawn_y(i) - ed.laid_y(i)) > 0.5f) ok = false;
    return ok;
}

static void key_one_frame(Editor& ed, ImGuiKey k) {  // key down for exactly one frame
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(k, true);
    frame(ed);
    io.AddKeyEvent(k, false);
}

static void test_layout_motion() {
    // Opening, closing and cutting lines: while space animates, lines never
    // overlap, and everything settles exactly on the layout.
    {
        Editor ed = fresh("a\nb\nc");
        key(ed, ImGuiKey_End);
        key_one_frame(ed, ImGuiKey_Enter);
        CHECK(settles_cleanly(ed));

        // A deleted line fades out while holding its space, so the line
        // below mustn't start rising into it (text on top of text).
        Editor d = fresh("a\nb\nc");
        key(d, ImGuiKey_DownArrow);
        float c0 = d.drawn_y(2);
        d.delete_lines();
        frame(d);
        CHECK(std::fabs(d.drawn_y(1) - c0) < 0.5f);
        CHECK(settles_cleanly(d));

        Editor s = fresh("one\ntwo\nthree\nfour");
        s.select_range({1, 0}, {2, 5});
        frame(s);
        s.cut();
        CHECK(settles_cleanly(s));
    }

    // Breaking a wrapped paragraph in half: the text is already where it
    // belongs, so the second half starts in place and nothing ever moves up
    // (a visible jump back-and-forth otherwise).
    {
        std::string para;
        for (int i = 0; i < 30; i++) para += "asdasd asda ";
        Editor b = fresh(para + "\nbelow");
        float below0 = b.drawn_y(1);
        b.select_range({0, (int)para.size() / 2}, {0, (int)para.size() / 2});
        frame(b);
        key_one_frame(b, ImGuiKey_Enter);
        bool never_up = std::fabs(b.drawn_y(1) - b.laid_y(1)) < 0.5f && b.drawn_y(2) >= below0 - 0.5f;
        float prev_half = b.drawn_y(1), prev_below = b.drawn_y(2);
        for (int i = 0; i < 40; i++) {
            frame(b);
            if (b.drawn_y(1) < prev_half - 0.5f || b.drawn_y(2) < prev_below - 0.5f) never_up = false;
            prev_half = b.drawn_y(1);
            prev_below = b.drawn_y(2);
        }
        CHECK(never_up);
        CHECK(settles_cleanly(b));
    }

    // Joining two wrapped paragraphs: what's below only ever moves up.
    {
        std::string para;
        for (int i = 0; i < 20; i++) para += "asdasd asda ";
        Editor j = fresh(para + "\n" + para + "\nbelow");
        j.select_range({1, 0}, {1, 0});
        frame(j);
        float prev = j.drawn_y(2);
        key_one_frame(j, ImGuiKey_Backspace);
        bool never_down = true;  // "below" is line 1 now
        for (int i = 0; i < 40; i++) {
            if (j.drawn_y(1) > prev + 0.5f) never_down = false;
            prev = j.drawn_y(1);
            frame(j);
        }
        CHECK(never_down);
        CHECK(settles_cleanly(j));
    }

    // After an idle pause the first frame has a huge delta time. The push
    // must not finish (snap) in that one frame, must not go haywire, and
    // must still settle.
    {
        Editor p = fresh("a\nb");
        key(p, ImGuiKey_End);
        g_dt_override = 0.8f;  // as if delta had been asleep for 0.8s
        key_one_frame(p, ImGuiKey_Enter);
        g_dt_override = 0.0f;
        CHECK(p.drawn_y(2) < p.laid_y(2) - 1.0f);
        CHECK(settles_cleanly(p) && p.text() == "a\n\nb");
    }

    // A new document appears as it is, with no motion left over from the old.
    Editor n = fresh("x\ny\nz\nw");
    key(n, ImGuiKey_End);
    key_one_frame(n, ImGuiKey_Enter);
    n.set_text("p\nq\nr");
    frame(n);
    CHECK(settles_cleanly(n, 0));
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

    // Lines opening and closing inside a block, faster than they settle:
    // the block's background never tears open mid-animation.
    Editor e = fresh("```\nfloat Test = 10.0;\n```\nafter");
    e.select_range({1, 5}, {1, 5});
    frame(e);
    bool whole = true;
    for (ImGuiKey k : {ImGuiKey_Enter, ImGuiKey_Backspace}) {
        for (int n = 0; n < 4; n++) {
            key_one_frame(e, k);
            for (int f = 0; f < 3; f++) {
                if (!no_gaps(code_bg_spans(marker))) whole = false;
                frame(e);
            }
        }
    }
    e.select_range({2, 0}, {2, 0});
    frame(e);
    e.delete_lines();
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

    // Multi-line paste highlights exactly the text it added.
    Editor m = fresh("start end");
    ImGui::SetClipboardText("one\ntwo\n");
    m.select_range({0, 6}, {0, 6});
    frame(m);
    m.paste();
    frame(m);
    CHECK(m.pulse_range(a, b) && is(a, 0, 6) && is(b, 2, 0));

    // Undo that only removes text leaves nothing to highlight (its range
    // would point past the text). Redo highlights what it brought back.
    Editor u = fresh("");
    type(u, "hello");
    u.undo();
    frame(u);
    CHECK(!u.pulse_range(a, b));
    u.redo();
    frame(u);
    CHECK(u.pulse_range(a, b) && is(a, 0, 0) && is(b, 0, 5));

    // Any later edit clears it (its positions would be stale).
    type(u, "!");
    CHECK(!u.pulse_range(a, b));
}

static void test_morph() {
    // Every kind of line through the editor <-> reader morph, both
    // directions and reversed mid-morph: the text is untouched, and back in
    // the editor the layout is exactly what it was, with nothing left moving.
    std::string long_item = "- a list item long enough to wrap onto another row, with **bold** and `code` and a [link](https://x.y) in it";
    std::string doc = "# Title\n\nPara with *em* and ==mark== and [[wiki|alias]].\n\n" + long_item +
                      "\n- [ ] task\n- [x] done\n1. first\n> quote line\n---\n```cpp\nint x = 1;\n```\n\n~~gone~~ end";
    Editor e = fresh(doc);
    std::vector<float> laid;
    for (int i = 0; i < (int)e.lines().size(); i++) laid.push_back(e.laid_y(i));
    for (int round = 0; round < 2; round++) {
        e.set_reader(!e.reader());
        for (int f = 0; f < 20; f++) frame(e);
    }
    e.set_reader(true);
    frame(e);
    frame(e);
    e.set_reader(false);
    bool same_layout = settles_cleanly(e);
    for (int i = 0; i < (int)laid.size(); i++)
        if (std::fabs(e.laid_y(i) - laid[i]) > 0.01f) same_layout = false;
    CHECK_EQ(e.text(), doc);
    CHECK(same_layout);
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
    e.set_reader(false);
    for (int f = 0; f < 30; f++) {
        frame(e);
        if (!same_glyphs(glyphs_in(ink), still)) same = false;
    }
    CHECK(same);
    T = saved;
}

static void test_find_replace() {
    // Smart case: lowercase matches any case, a capital makes it exact.
    // Stepping visits every match in order and wraps both ways.
    Editor e = fresh("foo bar Foo\nfoo end");
    e.find_begin();
    e.find_set("foo");
    CHECK_EQ(e.find_count(), 3);
    CHECK(e.anchor() == (TextPos{0, 0}) && e.cursor() == (TextPos{0, 3}));
    e.find_step(1);
    CHECK(e.anchor() == (TextPos{0, 8}));
    e.find_step(1);
    CHECK(e.anchor() == (TextPos{1, 0}));
    e.find_step(1);
    CHECK(e.anchor() == (TextPos{0, 0}));
    e.find_step(-1);
    CHECK(e.anchor() == (TextPos{1, 0}));
    e.find_set("Foo");
    CHECK_EQ(e.find_count(), 1);

    // Find starts at the caret, not at the top of the note; when nothing
    // matches, the caret goes back to where find started.
    Editor c = fresh("cat\ncat");
    c.select_range({0, 1}, {0, 1});
    c.find_begin();
    c.find_set("cat");
    CHECK(c.anchor() == (TextPos{1, 0}));
    c.find_set("zzz");
    CHECK_EQ(c.find_count(), 0);
    CHECK(!c.has_selection() && c.cursor() == (TextPos{0, 1}));

    // Matches don't overlap.
    Editor o = fresh("aaaa");
    o.find_begin();
    o.find_set("aa");
    CHECK_EQ(o.find_count(), 2);

    // Replace one: only the current match changes; one undo takes it back.
    Editor r = fresh("a cat, a cat, a cat");
    r.find_begin();
    r.find_set("cat");
    r.replace_current("dog");
    CHECK_EQ(r.text(), std::string("a dog, a cat, a cat"));
    CHECK_EQ(r.selected_text(), std::string("cat"));  // on to the next one
    r.undo();
    CHECK_EQ(r.text(), std::string("a cat, a cat, a cat"));

    // Replace all, shrinking, growing and deleting, several matches to a
    // line: exact text, one undo step for the lot, and redo.
    const std::string src = "one two one\ntwo one one\nnone";
    struct { const char* q; const char* with; int n; const char* want; } cases[] = {
        {"one", "1", 5, "1 two 1\ntwo 1 1\nn1"},
        {"one", "one one", 5, "one one two one one\ntwo one one one one\nnone one"},
        {"one", "", 5, " two \ntwo  \nn"},
        {"zzz", "x", 0, "one two one\ntwo one one\nnone"},
    };
    for (auto& k : cases) {
        Editor a = fresh(src);
        a.find_begin();
        a.find_set(k.q);
        CHECK_EQ(a.replace_all(k.with), k.n);
        CHECK_EQ(a.text(), std::string(k.want));
        if (k.n == 0) continue;
        a.undo();
        CHECK_EQ(a.text(), src);
        a.redo();
        CHECK_EQ(a.text(), std::string(k.want));
    }
}

static void test_navigation_pass() {
    // Headings: lands on the heading's text; a "#" line inside a code block
    // isn't one; past the last heading it stays put.
    Editor h = fresh("# One\ntext\n## Two\n```\n# not a heading\n```\n# Three");
    h.goto_heading(1);
    CHECK(h.cursor() == (TextPos{2, 3}));
    h.goto_heading(1);
    CHECK(h.cursor() == (TextPos{6, 2}));
    h.goto_heading(1);
    CHECK(h.cursor() == (TextPos{6, 2}));
    h.goto_heading(-1);
    CHECK(h.cursor() == (TextPos{2, 3}));

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

    // A number growing a digit keeps the caret on the same character.
    std::string many;
    for (int i = 1; i <= 9; i++) many += std::to_string(i) + ". i\n";
    Editor w = fresh(many + "9. last");
    w.select_range({9, 7}, {9, 7});
    frame(w);
    w.duplicate_lines(-1);  // renumbers the list: the last two become 10, 11
    CHECK_EQ(w.lines()[9], std::string("10. last"));
    CHECK_EQ(w.lines()[10], std::string("11. last"));
    CHECK(w.cursor() == (TextPos{9, 8}));
}

// Undo and redo: every step restores the exact text, and the history is
// bounded without losing the newest steps.
static void test_undo_history() {
    // Step back through distinct edits and forward again.
    Editor e = fresh("base");
    std::vector<std::string> states = {e.text()};
    e.insert_text({0, 4}, " one");
    states.push_back(e.text());
    e.insert_text({0, 0}, "top\n");
    states.push_back(e.text());
    e.select_range({1, 2}, {1, 2});
    e.delete_lines();
    states.push_back(e.text());
    for (int i = (int)states.size() - 2; i >= 0; i--) {
        e.undo();
        CHECK_EQ(e.text(), states[i]);
        if (i == 2) CHECK(e.cursor() == (TextPos{1, 2}));  // the caret comes back too
    }
    e.undo();  // nothing left: a no-op
    CHECK_EQ(e.text(), states[0]);
    for (size_t i = 1; i < states.size(); i++) {
        e.redo();
        CHECK_EQ(e.text(), states[i]);
    }
    e.undo();
    e.undo();
    // A new edit drops what could be redone.
    e.insert_text({0, 0}, "new ");
    std::string after = e.text();
    e.redo();
    CHECK_EQ(e.text(), after);

    // Opening another note starts a fresh history: undo can't bring back
    // the previous note's text.
    Editor n = fresh("first note");
    n.insert_text({0, 0}, "x");
    n.set_text("second note");
    n.undo();
    CHECK_EQ(n.text(), std::string("second note"));

    // A reload from disk is undoable.
    Editor d = fresh("mine");
    d.reload_text("theirs\nchanged");
    CHECK_EQ(d.text(), std::string("theirs\nchanged"));
    d.undo();
    CHECK_EQ(d.text(), std::string("mine"));

    // How far back undo goes, and where it lands.
    auto depth = [](Editor& ed) {
        int n = 0;
        for (uint64_t v = ed.version(); ed.undo(), ed.version() != v; v = ed.version()) n++;
        return n;
    };
    // Many small edits: a long history is kept, capped, and it's the oldest
    // steps that go. Each edit adds one "x" at the start, so the length says
    // which step undo reached.
    const int kSmall = 700;
    Editor s = fresh("");
    for (int i = 0; i < kSmall; i++) s.insert_text({0, 0}, "x");
    s.undo();
    CHECK_EQ((int)s.text().size(), kSmall - 1);
    int ds = 1 + depth(s);
    CHECK(ds >= 100 && ds < kSmall);
    CHECK_EQ((int)s.text().size(), kSmall - ds);

    // A big note: fewer steps fit, but the newest are still there.
    const int kBig = 48;
    const std::string big((size_t)4 << 20, 'b');
    Editor b = fresh(big);
    for (int i = 0; i < kBig; i++) b.insert_text({0, 0}, "x");
    b.undo();
    CHECK_EQ(b.text().size(), big.size() + kBig - 1);
    int db = 1 + depth(b);
    CHECK(db >= 2 && db < kBig);
    CHECK_EQ(b.text().size(), big.size() + kBig - db);
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
