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
    test_layout_motion();
    test_code_block_space();
    test_change_pulse();
    test_morph();
    test_morph_still_text();

    ImGui::DestroyContext();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
