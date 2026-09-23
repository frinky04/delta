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

static void frame(Editor& ed, bool reader_keys = true) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = 1.0f / 60;
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
    CHECK_EQ(at(2), std::string("expected a number"));
    CHECK_EQ(at(3), std::string("expected true or false"));
    CHECK_EQ(at(4), std::string("unknown setting"));
    CHECK(at(5).rfind("out of range", 0) == 0);
    CHECK(at(8).rfind("unknown section", 0) == 0);
    CHECK_EQ(cfg.tab_size, 8);
    CHECK_EQ(cfg.line_of("colors.accent"), 7);
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
    test_smear();

    ImGui::DestroyContext();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
