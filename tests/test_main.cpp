// Headless tests: drives the real Editor through ImGui frames with simulated
// input. No window or GPU; font textures are requested but never uploaded.
#include <cstdio>
#include <fstream>
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <climits>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
#include "fonts.h"
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
#include "commands.h"
#include "tags.h"
#include "complete.h"

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

// A scratch folder under the system temp folder, emptied when made and
// removed when it goes out of scope. Nothing is created: what a test writes
// there makes its own folders. The name carries this process's id, so two
// test runs at once never clear each other's folders.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name)
        : path(fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(getpid()))) {
        clear();
    }
    ~TempDir() { clear(); }
    void clear() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Text split at '\n', a trailing newline giving a last empty line.
static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out(1);
    for (char ch : text)
        if (ch == '\n') out.emplace_back();
        else out.back().push_back(ch);
    return out;
}

static void test_markdown() {
    LineInfo info;
    std::vector<Style> st;

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

    // A whole note: fences and what's between them are code, and code has no links.
    std::vector<std::string> note = {"[[a]] #t", "```", "[[b]] #u", "~~~", "x https://c.d", "```", "plain"};
    CHECK(md_code_lines(note) == std::vector<bool>({false, true, true, true, false, true, true}));
    auto all = md_links(note);
    CHECK_EQ((int)all.size(), 3);
    if (all.size() == 3) {
        CHECK(all[0].line == 0 && all[0].link.wiki && all[0].link.target == "a");
        CHECK(all[1].line == 0 && all[1].link.tag && all[1].link.target == "t");
        CHECK(all[2].line == 4 && !all[2].link.wiki && all[2].link.target == "https://c.d");
    }
    all = md_links({"x https://c.d y", "[t](u) and #v"});
    CHECK_EQ((int)all.size(), 3);
    if (all.size() == 3) CHECK(all[0].line == 0 && all[1].line == 1 && all[1].link.target == "u" && all[2].link.tag);
    CHECK(md_word_char('a') && md_word_char('7') && md_word_char(0xC3) && !md_word_char('_') && !md_word_char('#'));

    // Reader text drops the syntax, and its map always points back into the
    // source in order (clicks and selections in the reader rely on it).
    std::string out;
    std::vector<Style> ost;
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
    std::vector<Style> st;
    std::string short_or_not_mono;
    for (const char* s : {"", "   ", "[", "[x", "]", "=", "= ;", " ; c", "a=b ;c", "[editor] ; c", "k = #12345"}) {
        ini_analyze(s, info, st);
        bool mono = std::all_of(st.begin(), st.end(), [](Style f) { return (f & S_CodeBlock) != 0; });
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

static float contrast(unsigned a, unsigned b) {
    auto lum = [](unsigned rgb) {
        float out = 0;
        for (auto [shift, w] : {std::pair{16, 0.2126f}, {8, 0.7152f}, {0, 0.0722f}}) {
            float c = ((rgb >> shift) & 0xFF) / 255.0f;
            out += w * (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f));
        }
        return out;
    };
    float x = lum(a), y = lum(b);
    return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f);
}

static void test_themes() {
    std::set<std::string> names;
    for (auto& t : all_themes()) {
        // Names are written in the config as they are: lowercase, no spaces.
        CHECK(names.insert(t.name).second);
        CHECK(std::all_of(t.name.begin(), t.name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }));
        Config c;
        std::vector<ConfigIssue> is;
        parse_config("[appearance]\ntheme = " + t.name + "\n", c, is);
        CHECK(is.empty() && c.theme == t.name);

        // Readable: text and secondary text as text (4.5:1), the caret as a mark (3:1).
        unsigned bg = t.colours[0], dim = t.colours[4], text = t.colours[5], accent = t.colours[7];
        if (contrast(text, bg) < 4.5f || contrast(dim, bg) < 4.5f || contrast(accent, bg) < 3.0f)
            { CHECK(!"theme contrast"); std::printf("   %s: text %.1f dim %.1f accent %.1f\n", t.name.c_str(), contrast(text, bg), contrast(dim, bg), contrast(accent, bg)); }
    }
    CHECK(find_theme("dark") == &all_themes()[0] && find_theme("light") && !find_theme("Dark") && !find_theme(""));
    CHECK_EQ(theme_colour("light", "accent"), std::string("#c96f0e"));
    CHECK_EQ(theme_colour("nope", "background"), std::string("#0c0c0c"));  // delta's dark
    CHECK_EQ(theme_colour("dark", "nope"), std::string());
    CHECK_EQ(find_setting("colors", "text")->default_for("light"), std::string("#2e2e2e"));

    // A mistyped theme names the nearest one rather than all of them.
    Config c;
    std::vector<ConfigIssue> is;
    parse_config("[appearance]\ntheme = serika dark\n", c, is);
    CHECK(c.theme == "dark" && is.size() == 1);
    if (!is.empty()) CHECK_EQ(is[0].msg, std::string("no theme with this name; did you mean serika_dark? using dark"));

    // [colors] goes on top of any theme, and dark follows the background.
    parse_config("[appearance]\ntheme = nord\n[colors]\naccent = #123456\nbackground = #ffffff\n", c, is);
    build_theme(c);
    CHECK(col_rgb(T.accent) == 0x123456u);
    CHECK(col_rgb(T.text) == find_theme("nord")->colours[5]);
    CHECK(!T.dark);
    std::string light = "light";
    build_theme(c, &light);  // the picker's preview
    CHECK(col_rgb(T.text) == 0x2e2e2eu);
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
        for (auto& t : all_themes()) {
            Config c;
            std::vector<ConfigIssue> is;
            parse_config(std::string("[") + s.section + "]\n" + s.key + " = " + s.default_for(t.name) + "\n", c, is);
            if (!is.empty()) rejected += std::string(s.key) + "/" + t.name + " ";
        }
    CHECK_EQ(rejected, std::string());

    // Config's own initialisers agree with the table, and every setting but
    // a colour has a field to land in.
    std::string drifted;
    for (auto& s : all_settings())
        if ((s.kind == Setting::Kind::Colour) != std::holds_alternative<std::monostate>(s.field) ||
            s.read(Config{}) != s.def)
            drifted += std::string(s.key) + " ";
    CHECK_EQ(drifted, std::string());
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
                       "tab_size = -3\nblank_line_height = nan\n[\n]\n[editor\nautosave_delay = inf\n";
    junk += std::string("font_size = \0\x01\xff", 15) + "\n" + std::string(100000, 'x') + " = 1\n";
    Config g;
    std::vector<ConfigIssue> gi;
    parse_config(junk, g, gi);
    CHECK(!gi.empty());
    std::string out_of_range;
    for (auto& s : all_settings()) {
        if (s.kind != Setting::Kind::Float && s.kind != Setting::Kind::Int) continue;
        float v = std::stof(s.read(g));
        if (!(v >= s.lo && v <= s.hi)) out_of_range += std::string(s.key) + " ";
    }
    CHECK_EQ(out_of_range, std::string());

    // From disk: a missing file is reported and changes nothing; a real one
    // (CRLF, as Windows editors save it) loads.
    TempDir disk_dir("delta_test_config");
    fs::create_directories(disk_dir.path);
    fs::path p = disk_dir.path / "config.ini";
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
    for (auto doc : {split_lines(default_config_text()), std::vector<std::string>{""},
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
    TempDir state("delta_test_state");
    fs::path kvp = state.path / "state.txt";
    std::map<std::string, std::string> kv = {{"last_note", "C:/My Notes/a = b.md"}, {"window", "1280 800"}, {"empty", ""}};
    save_kv(kvp, kv);  // creates the folder
    CHECK(load_kv(kvp) == kv);
}

#include "nav.h"
#include "problems.h"
#include "spell.h"
#include "spell_fix.h"
#include "speller.h"
#include "store.h"

#include <chrono>
#include <thread>

// Notes are the user's documents: every store operation must keep their
// bytes intact and refuse anything that would overwrite or orphan one.
static void test_store() {
    TempDir tmp("delta_test_store");
    const fs::path& root = tmp.path;
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
    for (auto& line : split_lines(text)) {
        std::vector<ImageRef> refs;
        if (md_image_row(line, &refs))
            for (auto& r : refs) out.push_back(r.src);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Every line holding an image is still a whole image line: no token cut in
// half, none run into text or into its neighbour.
static bool image_lines_valid(const std::string& text) {
    for (auto& line : split_lines(text))
        if (line.find("![") != std::string::npos && !md_image_row(line)) return false;
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
    std::vector<Style> st;
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

    TempDir tmp("delta_test_images");
    const fs::path& root = tmp.path;
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

    // The text changing under an open find (a reload from disk, an undo)
    // must not leave replace working on matches that are gone.
    Editor s = fresh("cat cat long line here");
    s.find_begin();
    s.find_set("cat");
    s.reload_text("x");
    CHECK_EQ(s.replace_all("dog"), 0);
    CHECK_EQ(s.text(), std::string("x"));
    s.reload_text("a cat");
    s.replace_current("dog");
    CHECK_EQ(s.text(), std::string("a dog"));
    s.undo();
    CHECK_EQ(s.replace_all("cow"), 1);
    CHECK_EQ(s.text(), std::string("a cow"));
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

static void test_link_problems() {
    std::string space, name;
    CHECK(Store::split_target(" work / Plan #goals ", space, name));
    CHECK_EQ(space, std::string("work"));
    CHECK_EQ(name, std::string("Plan"));
    CHECK(!Store::split_target("#only a heading", space, name));

    // The index comes from the disk: spaces and their notes, as written.
    TempDir tmp("delta_test_links");
    Store st;
    st.set_root(tmp.path);
    NoteRef made;
    st.create_note("work", "Plan", "", made);
    st.create_note("home", "Groceries", "", made);
    NoteIndex ix = st.index();
    CHECK(ix.has("", "plan") && ix.has("WORK", "Plan") && !ix.has("home", "Plan"));
    CHECK(ix == st.index());
    st.create_note("home", "Garden", "", made);
    CHECK(!(ix == st.index()));  // a new note is noticed

    auto probs = link_problems({
        "[[Plan]] and [[work/plan|the plan]] are fine",
        "[[Plam]] is a typo, [[nothing like it]] isn't",
        "`[[in code]]` and [[#heading only]]",
        "```",
        "[[fenced]]",
        "```",
        "[[hme/Groceries]] [[work/Groceries]]",
    }, ix);
    CHECK_EQ(probs.size(), (size_t)4);
    if (probs.size() == 4) {
        CHECK(probs[0].line == 1 && probs[0].col0 == 2 && probs[0].col1 == 6);  // just the target
        CHECK_EQ(probs[0].msg, std::string("no note with this name yet; did you mean Plan?"));
        CHECK_EQ(probs[1].msg, std::string("no note with this name yet"));
        CHECK_EQ(probs[2].line, 6);
        CHECK_EQ(probs[2].msg, std::string("no space with this name yet; did you mean home?"));
        CHECK_EQ(probs[3].msg, std::string("no note with this name yet"));  // it's in home, not work
    }

    // Several marks on a line, errors and spelling, draw without trouble and
    // clear away.
    Editor e = fresh("one two three");
    e.set_diagnostics({{0, 0, 3, "first"}, {0, 4, 7, "second"}, {0, 8, 13, "", Diagnostic::Kind::Spelling}});
    frame(e);
    e.set_diagnostics({});
    for (int i = 0; i < 60; i++) frame(e);
}

// Back and forward, recent notes, links here and the trash.
static void test_nav() {
    auto note = [](const char* name) {
        NoteRef n;
        n.space = "s";
        n.name = name;
        n.path = fs::path("s") / (std::string(name) + ".md");
        return n;
    };
    auto at = [&](const char* name, int line) { return Place{note(name), {line, 0}, 0}; };
    auto yes = [](const Place&) { return true; };
    auto where = [](const Place* p) { return p ? p->note.name + ":" + std::to_string(p->caret.line) : std::string("-"); };

    // A place is kept as you left it; forward undoes back.
    History h;
    h.jumped(Place{}, at("a", 0));           // opened a, nothing open before
    h.jumped(at("a", 7), at("b", 0));        // opened b from line 7 of a
    h.jumped(at("b", 3), at("b", 40));       // a heading in b
    CHECK_EQ(h.size(), 3);
    CHECK_EQ(where(h.step(-1, at("b", 41), yes)), std::string("b:3"));
    CHECK_EQ(where(h.step(-1, at("b", 3), yes)), std::string("a:7"));
    CHECK_EQ(where(h.step(-1, at("a", 7), yes)), std::string("-"));
    CHECK_EQ(where(h.step(1, at("a", 7), yes)), std::string("b:3"));
    CHECK_EQ(where(h.step(1, at("b", 3), yes)), std::string("b:41"));

    // Going nowhere adds nothing; going somewhere new ends forward.
    h.jumped(at("b", 41), at("b", 41));
    CHECK_EQ(h.size(), 3);
    h.step(-1, at("b", 41), yes);
    h.jumped(at("b", 3), at("c", 0));
    CHECK_EQ(h.size(), 3);
    CHECK_EQ(where(h.step(1, at("c", 0), yes)), std::string("-"));

    // A run of steps of one kind is one jump; anything between splits it.
    History r;
    r.jumped(Place{}, at("a", 0));
    r.jumped(at("a", 0), at("a", 10), History::Heading);
    r.jumped(at("a", 10), at("a", 20), History::Heading);
    r.jumped(at("a", 20), at("a", 30), History::Heading);
    CHECK_EQ(r.size(), 2);
    r.jumped(at("a", 31), at("a", 40), History::Heading);  // moved in between
    CHECK_EQ(r.size(), 3);
    CHECK_EQ(where(r.step(-1, at("a", 40), yes)), std::string("a:31"));
    CHECK_EQ(where(r.step(-1, at("a", 31), yes)), std::string("a:0"));

    // Places in a deleted note are skipped, or forgotten.
    History d;
    d.jumped(Place{}, at("a", 0));
    d.jumped(at("a", 0), at("gone", 0));
    d.jumped(at("gone", 0), at("c", 0));
    CHECK_EQ(where(d.step(-1, at("c", 0), [](const Place& p) { return p.note.name != "gone"; })), std::string("a:0"));
    d.forget(note("a").path);
    CHECK_EQ(d.size(), 1);
    // Somewhere history didn't see you go counts as where you are.
    CHECK_EQ(where(d.step(-1, at("x", 5), yes)), std::string("c:0"));

    Recent rc;
    rc.visit(note("a"));
    rc.visit(note("b"));
    rc.visit(note("a"));
    CHECK(rc.list().size() == 2 && rc.list()[0].name == "a" && rc.list()[1].name == "b");
    rc.update([](NoteRef& n) { if (n.name == "b") n.name = "b2"; });
    rc.forget(note("a").path);
    CHECK(rc.list().size() == 1 && rc.list()[0].name == "b2");

    // Links here resolve as following them does.
    TempDir tmp("delta_test_nav");
    Store st;
    st.set_root(tmp.path);
    NoteRef plan, home_plan, other;
    st.create_note("work", "Plan", "", plan);
    st.create_note("home", "Plan", "", home_plan);
    st.create_note("home", "Other", "", other);
    NoteIndex ix = st.index();
    std::string sp, nm;
    CHECK(ix.resolve("work", "", "plan", sp, nm) && sp == "work" && nm == "Plan");
    CHECK(ix.resolve("home", "", "plan", sp, nm) && sp == "home");
    CHECK(ix.resolve("home", "WORK", "plan", sp, nm) && sp == "work");
    CHECK(!ix.resolve("work", "", "nothing", sp, nm));
    std::vector<std::string> text = {"[[Plan]] and [[work/plan|alias]]", "```", "[[Plan]]", "```"};
    auto hits = links_to(text, "home", "work", "Plan", ix);  // [[Plan]] in home is home's own
    CHECK(hits.size() == 1 && hits[0].line == 0 && hits[0].col0 == 15 && hits[0].col1 == 24);
    CHECK_EQ(links_to(text, "work", "work", "Plan", ix).size(), (size_t)2);

    // Deleted notes come back, under a free name if theirs was taken.
    CHECK_EQ(st.trash_note(plan), std::string());
    NoteRef again, back;
    st.create_note("work", "Plan", "new", again);
    auto trashed = st.trashed();
    CHECK(trashed.size() == 1 && trashed[0].space == "work" && trashed[0].name == "Plan");
    if (trashed.size() == 1) {
        CHECK_EQ(st.restore_note(trashed[0], back), std::string());
        CHECK(back.name == "Plan (2)" && st.exists("work", "Plan (2)") && st.trashed().empty());
    }
}

// The words spellcheck would look at in a line, joined with |.
static std::string spelled(const std::string& line) {
    std::string out;
    for (auto& w : spell_words(line)) out += (out.empty() ? "" : "|") + spell_form(line, w);
    return out;
}

static void test_spell_words() {
    auto words = spell_words("hi there");
    CHECK(words.size() == 2 && words[1].begin == 3 && words[1].end == 8);

    // Prose, in all its shapes.
    CHECK_EQ(spelled("The quick brown fox."), std::string("The|quick|brown|fox"));
    CHECK_EQ(spelled("don't, don\xE2\x80\x99t and 'quoted' words"), std::string("don't|don't|and|quoted|words"));
    CHECK_EQ(spelled("well-known em\xE2\x80\x94" "dash"), std::string("well|known|em|dash"));
    CHECK_EQ(spelled("caf\xC3\xA9 na\xC3\xAFve \xF0\x9F\x8E\x89party"), std::string("caf\xC3\xA9|na\xC3\xAFve|party"));
    CHECK_EQ(spelled("# Heading here"), std::string("Heading|here"));
    CHECK_EQ(spelled("- [ ] buy milk"), std::string("buy|milk"));
    CHECK_EQ(spelled("1. first item"), std::string("first|item"));
    CHECK_EQ(spelled("> quoted text"), std::string("quoted|text"));
    CHECK_EQ(spelled("**bold** and *italic* ==marked=="), std::string("bold|and|italic|marked"));

    // Not prose: code, links' targets, addresses, names.
    CHECK_EQ(spelled("`code here` then text"), std::string("then|text"));
    CHECK_EQ(spelled("[link text](http://x.com/page) and https://example.com/foo bar"), std::string("link|text|and|bar"));
    CHECK_EQ(spelled("[[Some Note]] and [[target|shown words]]"), std::string("and|shown|words"));
    CHECK_EQ(spelled("mail me@example.com or @alice about #tag"), std::string("mail|or|about"));
    CHECK_EQ(spelled("see C:\\dir\\file or src/main.cpp and file.txt, e.g. this"), std::string("see|or|and|this"));
    CHECK_EQ(spelled("snake_case camelCase iPhone NASA HTML5 3rd mp3 x I"), std::string());
    CHECK_EQ(spelled("<span class=\"x\">inside</span> after"), std::string("inside|after"));
    CHECK_EQ(spelled("![alt text](img.png)"), std::string());
    CHECK_EQ(spelled("---"), std::string());

    // Whole lines skipped: front matter and fenced code.
    auto skipped = [](std::vector<std::string> lines) {
        std::string out;
        for (bool b : spell_skipped(lines)) out += b ? '1' : '0';
        return out;
    };
    CHECK_EQ(skipped({"---", "title: x", "---", "prose", "```", "code", "```", "after"}), std::string("11101110"));
    CHECK_EQ(skipped({"---", "prose"}), std::string("00"));  // unclosed: just a rule
    CHECK_EQ(skipped({"prose", "---", "more", "---"}), std::string("0000"));  // not at the top
}

// Polls `done` until it's true or 10 seconds pass, for work on another thread.
static bool wait_until(const std::function<bool()>& done) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

static bool wait_ready(Speller& sp) {
    return wait_until([&] { return sp.ready(); });
}

static void test_speller() {
    Speller sp;
    CHECK(!sp.ready() && !sp.loading());
    sp.load({"en_US", "xx_XX"}, {}, {});
    CHECK(sp.missing() == std::vector<std::string>{"xx_XX"});
    CHECK(wait_ready(sp));
    CHECK(sp.correct("hello") && sp.correct("Hello") && sp.correct("don't") && sp.correct("color"));
    CHECK(!sp.correct("helo") && !sp.correct("colour"));

    // Marks for a note: prose only, by line and byte range.
    std::vector<Diagnostic> d;
    CHECK(sp.check({"Teh cat sat.", "```", "teh code", "```", "Fine words here", "and teh"}, d, 1.0));
    CHECK_EQ(d.size(), (size_t)2);
    if (d.size() == 2) {
        CHECK(d[0].line == 0 && d[0].col0 == 0 && d[0].col1 == 3 && d[0].kind == Diagnostic::Kind::Spelling);
        CHECK(d[1].line == 5 && d[1].col0 == 4 && d[1].col1 == 7);
    }
    // Out of time, it still gets through a new line each call, and says
    // when it's done.
    std::vector<std::string> fresh_lines = {"alpha wrongg", "beta wrongg", "gamma wrongg"};
    int calls = 1;
    while (!sp.check(fresh_lines, d, 0.0) && calls < 10) calls++;
    CHECK_EQ(calls, 3);
    CHECK_EQ(d.size(), (size_t)3);

    // Suggestions, best first.
    auto sug = sp.suggest("recieve");
    CHECK(!sug.empty() && sug[0] == "receive");
    // Capitalised, as its lower-case self would be (hunspell alone: "Te", "Shier").
    CHECK(!sp.suggest("Teh").empty() && sp.suggest("Teh")[0] == "The");
    CHECK(!sp.suggest("Thier").empty() && sp.suggest("Thier")[0] == "Their");

    // Without waiting: nothing yet, the worker finds them, then they're there.
    CHECK(!sp.suggest_later("Seperately"));
    wait_until([&] { return !sp.suggesting(); });
    auto later = sp.suggest_later("Seperately");
    CHECK(later && !later->empty() && (*later)[0] == "Separately");
    // Checking never waits for it: new lines come through, if not at once.
    sp.suggest_later("Acommodate");
    std::vector<Diagnostic> marks;
    wait_until([&] { return sp.check({"a fresh line with a wurd"}, marks, 1.0); });
    CHECK(marks.size() == 1);

    // Accents the dictionary leaves off.
    CHECK(sp.correct("caf\xC3\xA9") && sp.correct("na\xC3\xAFve") && sp.correct("r\xC3\xA9sum\xC3\xA9") && sp.correct("Caf\xC3\xA9"));
    CHECK(!sp.correct("cafz\xC3\xA9"));

    // British, and both at once.
    sp.load({"en_GB"}, {}, {});
    CHECK(wait_ready(sp));
    CHECK(sp.correct("colour") && sp.correct("organise") && !sp.correct("color"));
    sp.load({"en_US", "en_GB"}, {}, {});
    CHECK(wait_ready(sp));
    CHECK(sp.correct("colour") && sp.correct("color") && !sp.correct("helo"));

    // Your own words: given at load, or added later; capitalised at the
    // start of a sentence too. Answers change, and the generation says so.
    sp.load({"en_US"}, {}, {"deltanote"});
    CHECK(wait_ready(sp));
    CHECK(sp.correct("deltanote") && sp.correct("Deltanote") && !sp.correct("frobnicat"));
    uint64_t gen = sp.generation();
    CHECK(sp.check({"a frobnicat here"}, d, 1.0) && d.size() == 1);
    sp.add_word("frobnicat");
    CHECK(sp.generation() != gen);
    CHECK(sp.check({"a frobnicat here"}, d, 1.0) && d.empty());

    // Off: nothing is checked.
    sp.unload();
    CHECK(!sp.ready() && !sp.loading());

    // Dictionaries of your own, found by name in a folder. An ISO8859-1 one
    // is read as UTF-8 (flags past ASCII included); other encodings are
    // refused, saying why.
    TempDir tmp("delta_test_dicts");
    const fs::path& dir = tmp.path;
    fs::create_directories(dir);
    atomic_write(dir / "xx_Latin.aff", "SET ISO8859-1\nPFX \xC5 Y 1\nPFX \xC5 0 re .\n", nullptr);
    atomic_write(dir / "xx_Latin.dic", "2\ncaf\xE9/\xC5\nword\n", nullptr);
    atomic_write(dir / "xx_Koi.aff", "SET KOI8-R\n", nullptr);
    atomic_write(dir / "xx_Koi.dic", "1\nword\n", nullptr);
    atomic_write(dir / "lonely.aff", "SET UTF-8\n", nullptr);  // no .dic: not a dictionary
    auto avail = Speller::available(dir);
    CHECK(avail == (std::vector<std::string>{"en_US", "en_GB", "xx_Koi", "xx_Latin"}));
    sp.load({"XX_latin", "xx_Koi"}, dir, {});
    CHECK(wait_ready(sp));
    CHECK(sp.correct("caf\xC3\xA9") && sp.correct("recaf\xC3\xA9") && sp.correct("word") && !sp.correct("wurd"));
    CHECK(sp.errors().size() == 1 && sp.errors()[0].find("KOI8-R") != std::string::npos);
}

static void test_spell_fix() {
    using K = Diagnostic::Kind;
    auto mark = [](int line, int a, int b) { return Diagnostic{line, a, b, "", K::Spelling}; };

    // Which word a fix is about: the one at the caret (either end counts),
    // else the nearest before it in its paragraph.
    std::vector<std::string> lines = {"one teh two", "", "para recieve and wrongg here", "more"};
    std::vector<Diagnostic> marks = {mark(0, 4, 7), mark(2, 5, 12), mark(2, 17, 23)};
    CHECK_EQ(misspelling_at(marks, {2, 5}), 1);
    CHECK_EQ(misspelling_at(marks, {2, 12}), 1);
    CHECK_EQ(misspelling_at(marks, {2, 13}), -1);
    CHECK_EQ(misspelling_near(marks, lines, {2, 28}), 2);   // end of the line: wrongg
    CHECK_EQ(misspelling_near(marks, lines, {2, 14}), 1);   // between: the one behind
    CHECK_EQ(misspelling_near(marks, lines, {3, 2}), 2);    // next line, same paragraph
    CHECK_EQ(misspelling_near(marks, lines, {2, 0}), -1);   // not back into the paragraph above
    CHECK_EQ(misspelling_near(marks, lines, {1, 0}), 0);    // a blank line: the paragraph above

    // A fix pass: on from where it is to the end, then from the top back to
    // where it began, then over.
    std::vector<Diagnostic> pass = {mark(0, 0, 3), mark(0, 10, 14), mark(2, 3, 8), mark(4, 1, 5)};
    bool wrapped = false;
    TextPos origin{2, 3};
    CHECK_EQ(fix_pass_next(pass, origin, origin, wrapped), 2);
    CHECK_EQ(fix_pass_next(pass, {2, 8}, origin, wrapped), 3);
    CHECK_EQ(fix_pass_next(pass, {4, 5}, origin, wrapped), 0);
    CHECK(wrapped);
    CHECK_EQ(fix_pass_next(pass, {0, 3}, origin, wrapped), 1);
    CHECK_EQ(fix_pass_next(pass, {0, 14}, origin, wrapped), -1);  // skipped ones aren't gone back to

    // Where positions land when a word changes length.
    CHECK(after_replace({0, 20}, 0, 4, 3, 5) == (TextPos{0, 22}));  // after it: moved along
    CHECK(after_replace({0, 7}, 0, 4, 3, 5) == (TextPos{0, 9}));    // at its end: still at its end
    CHECK(after_replace({0, 5}, 0, 4, 3, 5) == (TextPos{0, 9}));    // inside: at the new end
    CHECK(after_replace({0, 4}, 0, 4, 3, 5) == (TextPos{0, 4}));    // at its start: stays
    CHECK(after_replace({1, 20}, 0, 4, 3, 5) == (TextPos{1, 20}));  // another line: stays

    // The quick fix's cycle: each guess undoes the last first, so one undo
    // gets the word back whichever guess is in.
    Editor e = fresh("and teh rest");
    e.replace_range({0, 4}, {0, 7}, "the");
    e.undo();
    e.replace_range({0, 4}, {0, 7}, "tech");
    CHECK_EQ(e.text(), std::string("and tech rest"));
    e.undo();
    CHECK_EQ(e.text(), std::string("and teh rest"));
}

static void test_spell_config() {
    Config c;
    std::vector<ConfigIssue> is;
    parse_config("[spell]\nenabled = no\nlanguages = en_GB, de_DE\nspace.Recipes = de_DE\nspace. = en_US\nlanguage = en_US\n", c, is);
    CHECK(!c.spell);
    CHECK_EQ(c.spell_languages, std::string("en_GB, de_DE"));
    CHECK(split_list(c.spell_languages) == (std::vector<std::string>{"en_GB", "de_DE"}));
    CHECK(c.spell_spaces.size() == 1 && c.spell_spaces["recipes"] == "de_DE");
    CHECK_EQ(is.size(), (size_t)2);
    if (is.size() == 2) {
        CHECK(is[0].line == 4 && is[0].msg == "name a space after space.");
        CHECK(is[1].line == 5 && is[1].msg == "unknown setting; did you mean languages?");
    }
    Config d;
    parse_config("", d, is);
    CHECK(d.spell && d.spell_languages == "en_US" && d.spell_spaces.empty());

    auto hints = config_hints({"[spell]", "space.recipes = de_DE", "languages = en_US"}, "dark", {});
    CHECK_EQ(hints[1], std::string("dictionaries for this space"));
    CHECK_EQ(hints[2], std::string("dictionaries, comma-separated (default)"));

    // One undoable swap, as a fix makes.
    Editor e = fresh("I recieve it");
    e.replace_range({0, 2}, {0, 9}, "receive");
    CHECK_EQ(e.text(), std::string("I receive it"));
    CHECK(e.cursor() == (TextPos{0, 9}) && !e.has_selection());
    e.undo();
    CHECK_EQ(e.text(), std::string("I recieve it"));
}

// delta's own words are lower case, and fragments rather than sentences:
// no capital to start, no full stop to end. Names the user chose keep their
// case, so only the text we write is checked. The command titles and status
// messages live in App, which isn't built into the tests, so its source is
// read instead.
static bool in_voice(const std::string& s) {
    return !s.empty() && !std::isupper((unsigned char)s[0]) && s.back() != '.';
}

static std::string read_src(const char* name) {
    std::ifstream f(std::string(DELTA_SRC_DIR) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_ui_voice() {
    CHECK_EQ(plain_msg("Access is denied.\r\n"), std::string("access is denied"));
    CHECK_EQ(plain_msg("OS error"), std::string("OS error"));
    CHECK_EQ(plain_msg(""), std::string());

    const std::string long_name(201, 'x');
    for (auto* what : {"note", "space"})
        for (const std::string& name : {std::string(), std::string(".x"), std::string("x."), std::string("a/b"), long_name})
            CHECK(in_voice(Store::check_name(name, what)));

    for (auto& s : all_settings())  // some need no description
        if (*s.desc && !in_voice(s.desc)) { CHECK(!"setting description in voice"); std::printf("   %s.%s: %s\n", s.section, s.key, s.desc); }

    const std::regex title(R"re(\badd\("[a-z_.]+", "([^"]*)")re");
    const std::regex said(R"re(\b(?:status|prompt|pick)\((.*))re");
    const std::regex literal(R"re("((?:[^"\\]|\\.)*)")re");
    int titles = 0;
    for (auto* file : {"app.cpp", "app_images.cpp"}) {
        std::string src = read_src(file);
        CHECK(!src.empty());
        std::istringstream lines(src);
        int n = 0;
        for (std::string line; std::getline(lines, line);) {
            n++;
            std::smatch m;
            if (std::regex_search(line, m, title)) {
                titles++;
                std::string t = m[1];
                bool ok = in_voice(t) && std::none_of(t.begin(), t.end(), [](char c) { return std::isupper((unsigned char)c); });
                if (!ok) { CHECK(!"command title in voice"); std::printf("   %s:%d: %s\n", file, n, t.c_str()); }
            }
            // Every literal on a status/prompt/pick line, so both arms of a ?: count.
            if (!std::regex_search(line, m, said)) continue;
            std::string rest = m[1];
            for (std::sregex_iterator it(rest.begin(), rest.end(), literal), end; it != end; ++it) {
                std::string t = (*it)[1];
                bool starts = t.empty() || !std::isalpha((unsigned char)t[0]) || !std::isupper((unsigned char)t[0]);
                bool ends = t.size() < 2 || t.back() != '.' || t[t.size() - 2] == '.';
                if (!starts || !ends) { CHECK(!"message in voice"); std::printf("   %s:%d: %s\n", file, n, t.c_str()); }
            }
        }
    }
    CHECK(titles > 50);  // the pattern still finds the command table
}

// One frame of the palette with `k` held through it, its rows from `build`.
// `first` runs inside the frame before the palette draws. Returns the
// action of the row chosen, if any.
static std::function<void()> palette_frame(Palette& p, ImGuiKey k, const Palette::Builder& build,
                                           const std::function<void()>& first = {}) {
    ImGuiIO& io = ImGui::GetIO();
    if (k != ImGuiKey_None) io.AddKeyEvent(k, true);
    io.DisplaySize = ImVec2(1000, 800);
    io.DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    if (first) first();
    Palette::Frame f{g_sans, g_sans, 17, 15, 1, 100, 600, 40, 700, 0, io.DeltaTime, Palette::Selection::Marker, 2, false};
    auto chosen = p.draw(f, build, [](char) { return std::string(); });
    ImGui::Render();
    if (k != ImGuiKey_None) io.AddKeyEvent(k, false);
    return chosen;
}

// The palette's modes: a scoped one (notes, search, tags) widens to every
// space and back, and no other does; backspace on an empty query
// leaves a mode for the root, commands.
static void test_palette_modes() {
    Palette p;
    int built = 0;
    auto step = [&](ImGuiKey k = ImGuiKey_None) {
        palette_frame(p, k, [&](const std::string&, std::vector<PaletteItem>&, int& total) { built++; total = 0; });
    };
    p.open(".");
    step();
    CHECK(p.in_mode('.') && p.scoped() && !p.wide());
    int builds = built;
    p.set_wide(true);  // palette.widen, bound to tab by App
    step();
    CHECK(p.wide() && built == builds + 1);  // widened, and the list built again
    p.set_wide(false);
    step();
    CHECK(!p.wide());

    p.open("@");
    p.set_wide(true);
    step();
    CHECK(p.in_mode('@') && !p.scoped() && !p.wide());

    p.open("?", true);
    step();
    CHECK(p.in_mode('?') && p.wide());

    p.open("#");
    step();
    CHECK(p.in_mode('#') && p.scoped());  // tags widen too

    p.open("");
    step();
    CHECK(p.in_mode(0) && !p.scoped() && !p.wide());  // commands have no scope

    p.open(".");
    step();
    step(ImGuiKey_Backspace);
    CHECK(p.in_mode(0));
    p.close();
    for (int i = 0; i < 30; i++) step();
}

// "open manual" shows the README as the build embedded it.
extern const unsigned char g_manual[];
extern const std::size_t g_manual_size;

// Searching the commands the way people do: words in any order, the group
// in the id, their own word for it. The titles are read from App's source,
// so this follows the real table.
static void test_command_search() {
    std::vector<std::pair<std::string, std::string>> cmds;  // id, title
    std::set<std::string> seen;
    const std::regex add(R"re(\badd\("([a-z_.]+)", "([^"]*)", ")re");
    std::string src = read_src("app.cpp");
    for (std::sregex_iterator it(src.begin(), src.end(), add), end; it != end; ++it)
        if (seen.insert((*it)[1]).second) cmds.push_back({(*it)[1], (*it)[2]});
    CHECK(cmds.size() > 50);

    auto top = [&](const std::string& q, std::string* via = nullptr) {
        std::string best;
        int best_score = INT_MIN;
        for (auto& [id, title] : cmds) {
            int score;
            std::string v;
            if (match_command(q, title, id, command_keywords(id), score, nullptr, &v) && score > best_score) {
                best = id, best_score = score;
                if (via) *via = v;
            }
        }
        return best;
    };
    struct Case { const char* q; const char* want; };
    for (Case c : {Case{"delete note", "note.delete"}, {"note delete", "note.delete"}, {"space rename", "space.rename"},
                   {"rename note", "note.rename"}, {"zoom reset", "view.zoom_reset"}, {"heading next", "nav.heading_next"},
                   {"backlinks", "palette.backlinks"}, {"settings", "config.open"}, {"trash", "note.restore"},
                   {"outline", "palette.headings"}, {"goto line", "palette.line"}, {"spell", "view.spelling"},
                   {"help", "app.manual"}, {"new", "note.new"}, {"find replace", "find.replace"},
                   {"fullscreen", "view.fullscreen"}, {"tgrm", "view.reader"}, {"tags", "palette.tags"},
                   {"rename tag", "tag.rename"}, {"widen", "palette.widen"}, {"sort", "palette.sort"}}) {
        std::string got = top(c.q);
        if (got != c.want) { CHECK(!"command search"); std::printf("   \"%s\": got %s, want %s\n", c.q, got.c_str(), c.want); }
    }

    // The keyword shows when it's what matched; a title word doesn't.
    std::string via;
    top("backlinks", &via);
    CHECK_EQ(via, std::string("backlinks"));
    top("rename note", &via);
    CHECK(via.empty());

    int score;
    std::vector<int> hl;
    CHECK(match_command("note ren", "rename note", "note.rename", "", score, &hl, nullptr));
    CHECK(hl.size() == 7);  // "note" and "ren", where they are in the title
    CHECK(!match_command("note zzz", "rename note", "note.rename", "", score, nullptr, nullptr));
}

// The note list's name order: case aside, numbers by value.
static void test_natural_sort() {
    std::vector<std::string> v = {"note 10", "Note 2", "note 1", "apple", "Banana", "2026-09-25", "2026-09-03", "a", "note 02b", "note 2a"};
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return natural_less(a, b); });
    std::string got;
    for (auto& s : v) got += (got.empty() ? "" : "|") + s;
    CHECK_EQ(got, std::string("2026-09-03|2026-09-25|a|apple|Banana|note 1|Note 2|note 2a|note 02b|note 10"));
    CHECK(!natural_less("x", "x") && natural_less("x", "xy") && !natural_less("xy", "x"));
}

static void test_manual() {
    std::string readme = read_src("../README.md");
    CHECK(!readme.empty());
    CHECK(std::string((const char*)g_manual, g_manual_size) == readme);
}

// --sandbox: a config pointing at a notes folder beside it, made once and
// then reused as it was left.
static void test_sandbox() {
    TempDir tmp("delta_test_sandbox");
    const fs::path& dir = tmp.path;
    std::error_code ec;
    std::string err;
    fs::path cfg = make_sandbox(dir, &err);
    CHECK(cfg == dir / "delta.ini" && err.empty());
    CHECK(fs::is_directory(dir / "notes"));
    Config c;
    std::vector<ConfigIssue> issues;
    CHECK(load_config(cfg, c, issues) && issues.empty());
    CHECK(fs::equivalent(expand_user(c.notes_dir), dir / "notes", ec));

    CHECK(atomic_write(cfg, "[editor]\nfont = Georgia\n", nullptr));
    CHECK(make_sandbox(dir, &err) == cfg);
    std::string text;
    CHECK(read_file(cfg, text) && text == "[editor]\nfont = Georgia\n");  // kept, not rewritten
}

// [editor] font and mono_font: the embedded font by default, else a font
// file or an installed family, and a problem (with the embedded font) when
// neither can be found.
static void test_fonts() {
    FontPick p;
    CHECK_EQ(pick_font("", "Noto Sans", p), std::string());
    CHECK(p.embedded());
    CHECK_EQ(pick_font("noto sans", "Noto Sans", p), std::string());  // the default, however it's cased
    CHECK(p.embedded());

    TempDir tmp("delta_test_fonts");
    const fs::path& root = tmp.path;
    fs::create_directories(root);
    fs::path ttf = root / "Mine.ttf", junk = root / "junk.ttf";
    CHECK(atomic_write(ttf, std::string((const char*)g_font_mono, g_font_mono_size), nullptr));
    CHECK(atomic_write(junk, "not a font", nullptr));

    CHECK_EQ(pick_font(path_u8(ttf), "Noto Sans", p), std::string());
    CHECK(p.regular.file == ttf && p.bold.file.empty());  // one file, one weight
    CHECK_EQ(pick_font(path_u8(root / "gone.ttf"), "Noto Sans", p), std::string("no font file here, using Noto Sans"));
    CHECK(p.embedded());
    CHECK_EQ(pick_font(path_u8(junk), "Noto Sans", p).rfind("not a font", 0), (size_t)0);
    CHECK(p.embedded());

#ifdef _WIN32
    // Families every Windows install has.
    CHECK_EQ(pick_font("consolas", "Noto Sans Mono", p), std::string());
    CHECK(!p.embedded() && fs::exists(p.regular.file));
    CHECK(!p.bold.file.empty() && p.bold.file != p.regular.file && p.bold.weight >= 600);
    CHECK_EQ(pick_font("Consolsa", "Noto Sans Mono", p), std::string("no installed font with this name; did you mean Consolas?"));
    CHECK(p.embedded());
    CHECK_EQ(pick_font("Zzqx Nothing Like It", "Noto Sans", p), std::string("no installed font with this name, using Noto Sans"));

    // The picker lists families with whether they're monospaced.
    auto families = system_font_families();
    auto family = [&](const char* name) -> const FontFamily* {
        for (auto& f : families)
            if (f.name == name) return &f;
        return nullptr;
    };
    CHECK(family("Consolas") && family("Consolas")->mono && real_bold(family("Consolas")->regular, family("Consolas")->bold));
    CHECK(family("Georgia") && !family("Georgia")->mono);
#endif

    CHECK(is_font_path("~/fonts/a.ttf") && is_font_path("Mine.OTF") && is_font_path("C:\\x\\y"));
    CHECK(!is_font_path("JetBrains Mono") && !is_font_path("Iosevka Term"));
}

// The font worker: fonts found and read off the UI thread, the newest request
// winning, and each file read once while anything holds it.
static void test_font_loader() {
    TempDir tmp("delta_test_font_loader");
    const fs::path& root = tmp.path;
    fs::create_directories(root);
    fs::path a = root / "a.ttf", b = root / "b.ttf";
    CHECK(atomic_write(a, std::string((const char*)g_font_mono, g_font_mono_size), nullptr));
    CHECK(atomic_write(b, std::string((const char*)g_font_sans, g_font_sans_size), nullptr));

    FontLoader fl;
    // Now, for start-up: the same file for both is read once.
    FontLoader::Pair now = fl.load_now({path_u8(a), "Noto Sans", path_u8(a), "Noto Sans Mono"});
    CHECK(now.sans.regular && now.sans.regular == now.mono.regular);
    CHECK_EQ(now.sans.regular->size(), g_font_mono_size);
    CHECK(!now.sans.bold && now.sans.problem.empty());

    // On the worker, the newest wins: the first is never handed over.
    fl.load({path_u8(b), "Noto Sans", "", "Noto Sans Mono"});
    fl.load({path_u8(a), "Noto Sans", "", "Noto Sans Mono"});
    FontLoader::Pair got;
    wait_until([&] { return fl.take(got); });
    CHECK_EQ(got.sans.value, path_u8(a));
    CHECK(got.sans.regular == now.sans.regular);  // still held (by `now`): not read again
    CHECK(!got.mono.regular && got.mono.problem.empty());  // the embedded font
    CHECK(!fl.take(got));  // once
    wait_until([&] { return !fl.busy(); });
    CHECK(!fl.busy());

    // Once nothing holds a file's bytes, they're gone; asked again, it's read again.
    std::weak_ptr<const std::string> gone = now.sans.regular;
    now = {};
    got = {};
    CHECK(gone.expired());
    FontLoader::Pair again = fl.load_now({path_u8(a), "Noto Sans", "", "Noto Sans Mono"});
    CHECK(again.sans.regular && again.sans.regular->size() == g_font_mono_size);

    // A problem comes back with the embedded font.
    fl.preview(path_u8(root / "missing.ttf"), "Noto Sans");
    FontLoad pl;
    wait_until([&] { return fl.take_preview(pl); });
    CHECK_EQ(pl.problem, std::string("no font file here, using Noto Sans"));
    CHECK(!pl.regular);

#ifdef _WIN32
    fl.list_families();
    std::vector<FontFamily> fams;
    wait_until([&] { return fl.take_families(fams); });
    CHECK(fams.size() > 10);
    bool sorted = true;
    for (size_t i = 1; i < fams.size(); i++)
        if (lower(fams[i - 1].name) > lower(fams[i].name)) sorted = false;
    CHECK(sorted);
#endif
}

// What the font picker writes: the value replaced where it's set, or the
// setting added where "add setting" would put it.
static void test_config_set() {
    auto set = [](const std::string& text, const std::string& key, const std::string& value) {
        std::vector<std::string> lines = split_lines(text);
        apply_edit(lines, config_set(lines, "editor", key, value));
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) out += (i ? "\n" : "") + lines[i];
        return out;
    };
    // Replaced, keeping a comment after it.
    CHECK_EQ(set("[editor]\nfont = Georgia ; mine\n", "font", "Consolas"), std::string("[editor]\nfont = Consolas ; mine\n"));
    CHECK_EQ(set("[editor]\nfont = \"Georgia\"\n", "font", "Consolas"), std::string("[editor]\nfont = Consolas\n"));
    // No value yet.
    CHECK_EQ(set("[editor]\nfont =\n", "font", "Consolas"), std::string("[editor]\nfont = Consolas\n"));
    CHECK_EQ(set("[editor]\nfont = ; later\n", "font", "Consolas"), std::string("[editor]\nfont = Consolas ; later\n"));
    // Set twice: the last one counts, so that's the one changed.
    CHECK_EQ(set("[editor]\nfont = A\nfont = B\n", "font", "C"), std::string("[editor]\nfont = A\nfont = C\n"));
    // Only in its own section, and not commented out.
    CHECK_EQ(set("[spell]\nfont = A\n[editor]\n; font = B\nfont_size = 14\n", "font", "C"),
             std::string("[spell]\nfont = A\n[editor]\n; font = B\nfont_size = 14\nfont = C\n"));
    // Added: after the section's last setting, or in a new section.
    CHECK_EQ(set("[editor]\nfont_size = 14\n\n[colors]\n", "mono_font", "Consolas"),
             std::string("[editor]\nfont_size = 14\nmono_font = Consolas\n\n[colors]\n"));
    CHECK_EQ(set("[general]\nx = 1\n", "font", "Georgia"), std::string("[general]\nx = 1\n\n[editor]\nfont = Georgia"));
    CHECK_EQ(set("", "font", "Georgia"), std::string("[editor]\nfont = Georgia"));
    // A " ;" in the value would start a comment, so it's quoted.
    CHECK_EQ(set("[editor]\n", "font", "C:/my ;fonts/a.ttf"), std::string("[editor]\nfont = \"C:/my ;fonts/a.ttf\"\n"));
}

// #tags: what counts as one, finding and renaming them, the palette's
// queries, and the index of which notes have which.
static void test_tags() {
    LineInfo info;
    std::vector<Style> st;
    std::vector<LinkSpan> links;
    auto tags = [&](const std::string& s) {
        links.clear();
        md_analyze(s, false, info, st, &links);
        std::string out;
        for (auto& l : links)
            if (l.tag) out += (out.empty() ? "" : ",") + l.target;
        return out;
    };
    CHECK_EQ(tags("#tag"), std::string("tag"));
    CHECK_EQ(tags("a #one and #two-three, #a/b."), std::string("one,two-three,a/b"));
    CHECK_EQ(tags("a#b c&#39; #123 ##x"), std::string());  // in a word, an entity, a number, a run of #
    CHECK_EQ(tags("#12a #\xC3\xBC" "ber"), std::string("12a,\xC3\xBC" "ber"));
    CHECK_EQ(tags("`#code` [x](u#frag) https://a.b/#y [[n#h]]"), std::string());
    CHECK_EQ(tags("## Title #inhead"), std::string("inhead"));
    CHECK_EQ(tags("- [ ] task #todo"), std::string("todo"));
    CHECK_EQ(tags("(#paren) #end/"), std::string("paren,end"));
    md_analyze("# Heading", false, info, st);
    CHECK(info.kind == LineKind::Heading);
    // A # on its own isn't a heading yet (it may become a tag); with its space it is.
    for (const char* s : {"#", "##", "  ###"}) {
        md_analyze(s, false, info, st);
        CHECK(info.kind != LineKind::Heading);
    }
    md_analyze("## ", false, info, st);
    CHECK(info.kind == LineKind::Heading && info.level == 2);
    md_analyze("x #tag", false, info, st);
    CHECK((st[2] & S_Tag) && (st[3] & S_Tag) && !(st[1] & S_Tag));  // the # is part of it
    std::string out;
    std::vector<Style> ost;
    std::vector<int> map;
    md_reader("x #tag", info, st, out, ost, map);
    CHECK_EQ(out, std::string("x #tag"));  // the reader keeps the #: it says what the word is

    // Every tag in a note, code skipped; a note's set keeps the first spelling.
    auto uses = find_tags({"#a and #B", "```", "#incode", "```", "#b"});
    CHECK_EQ(uses.size(), (size_t)3);
    if (uses.size() == 3) CHECK(uses[1].line == 0 && uses[1].col0 == 7 && uses[1].col1 == 9 && uses[1].name == "B");
    TagSet set = tags_of({"#a and #B", "#b"});
    CHECK(set.size() == 2 && set["b"] == "B");

    CHECK(check_tag("ok-tag/sub").empty());
    for (const char* bad : {"", "two words", "123", "a/", "a.b"}) {
        std::string why = check_tag(bad);
        CHECK(!why.empty() && in_voice(why));
    }

    // Renaming: any case, not in code, not another tag it's the start of;
    // the caret stays with its text.
    std::vector<std::string> lines = {"#Rust and #rust-lang #rust", "`#rust`", "```", "#rust", "```"};
    TextPos caret{0, 26};
    CHECK_EQ(rename_tag(lines, "rust", "language", &caret), 2);
    CHECK_EQ(lines[0], std::string("#language and #rust-lang #language"));
    CHECK(lines[1] == "`#rust`" && lines[3] == "#rust");
    CHECK_EQ(caret.col, 34);
    caret = {0, 3};  // "#la|nguage"
    rename_tag(lines, "language", "x", &caret);
    CHECK_EQ(lines[0], std::string("#x and #rust-lang #x"));
    CHECK_EQ(caret.col, 2);

    // Where a tag being written starts, for completing it.
    std::vector<Style> none;
    CHECK_EQ(tag_word_start("a #ru", 5, none), 3);
    CHECK_EQ(tag_word_start("a #", 3, none), 3);
    CHECK_EQ(tag_word_start("a #ru st", 5, none), 3);
    CHECK_EQ(tag_word_start("a #rust", 5, none), -1);  // not in the middle of one
    CHECK_EQ(tag_word_start("a#ru", 4, none), -1);
    CHECK_EQ(tag_word_start("#12", 3, none), -1);
    CHECK_EQ(tag_word_start("ab", 2, none), -1);
    md_analyze("`#ru`", false, info, st);
    CHECK_EQ(tag_word_start("`#ru`", 4, st), -1);
    // ...and a [[link]]'s target.
    CHECK_EQ(md_link_word_start("see [[pl", 8, none), 6);
    CHECK_EQ(md_link_word_start("see [[pl]]", 8, none), 6);
    CHECK_EQ(md_link_word_start("see [[", 6, none), 6);
    CHECK_EQ(md_link_word_start("[[a|b", 5, none), -1);
    CHECK_EQ(md_link_word_start("[[a]] b", 7, none), -1);
    CHECK_EQ(md_link_word_start("[[plan", 3, none), -1);

    // The palette's queries.
    TagQuery q = parse_tag_query("");
    CHECK(q.typing && q.word.empty() && q.want.empty());
    q = parse_tag_query("rust #idea -#done #dr");
    CHECK(q.want == std::vector<std::string>({"rust", "idea"}) && q.skip == std::vector<std::string>({"done"}));
    CHECK(q.typing && !q.typing_skip && q.word == "dr");
    q = parse_tag_query("Rust ");
    CHECK(!q.typing && q.want == std::vector<std::string>({"rust"}));
    q = parse_tag_query("rust -dr");
    CHECK(q.typing && q.typing_skip && q.word == "dr");
    TagSet has = {{"rust", "Rust"}, {"idea", "idea"}};
    CHECK(tag_query_match(parse_tag_query("rust #idea "), has));
    CHECK(!tag_query_match(parse_tag_query("rust -#idea "), has));
    CHECK(!tag_query_match(parse_tag_query("rust #done "), has));
    CHECK(tag_query_match(parse_tag_query("zz"), has));  // nothing finished yet
    CHECK_EQ(tag_query_fill("ru", "rust"), std::string("rust "));
    CHECK_EQ(tag_query_fill("rust #id", "idea"), std::string("rust #idea "));
    CHECK_EQ(tag_query_fill("rust -#do", "done"), std::string("rust -#done "));
    CHECK_EQ(tag_query_fill("rust ", "idea"), std::string("rust #idea "));

    // The index: read once, again when a note changes, forgotten when it goes.
    TempDir tmp("delta_test_tags");
    Store store;
    store.set_root(tmp.path);
    NoteRef a, b, c;
    store.create_note("work", "a", "#x #y", a);
    store.create_note("work", "b", "#y", b);
    store.create_note("home", "c", "#X", c);
    TagIndex ix;
    ix.refresh(store.all_notes());
    auto with = [&](const std::string& t) {
        int n = 0;
        for (auto& [path, e] : ix.notes()) n += e.tags.count(t) ? 1 : 0;
        return n;
    };
    CHECK(ix.notes().size() == 3 && with("x") == 2 && with("y") == 2);
    std::string err;
    CHECK(atomic_write(a.path, "#z", &err));
    ix.refresh(store.all_notes());
    CHECK(with("x") == 1 && with("z") == 1);
    fs::remove(c.path);
    ix.refresh(store.all_notes());
    CHECK(ix.notes().size() == 2 && with("x") == 0);
    ix.set(a, {"#w"});  // as it is in the editor
    CHECK(with("w") == 1 && with("z") == 0);

    // Counted: each tag's notes, under its name as first written.
    store.create_note("home", "d", "#Y #w", c);
    ix.refresh(store.all_notes());
    auto all = ix.tally();
    CHECK(all.size() == 2 && all["y"].name == "Y" && all["y"].notes == 2 && all["w"].notes == 2);
    CHECK(ix.notes_with("W") == 2 && ix.notes_with("nope") == 0);
    auto work = ix.tally([](const TagIndex::Entry& e) { return e.note.space == "work"; });
    CHECK(work.size() == 2 && work["y"].name == "y" && work["y"].notes == 1);
}

// Completion as you type: the list comes up for a word being written, enter
// takes the pick (unless the word is already exactly that), escape puts it
// away for that word, and moving off it closes it.
static void test_completion() {
    auto source = [](std::vector<std::string> names) {
        return [names](const std::string& w) {
            std::vector<CompletionItem> out;
            for (auto& n : names) {
                CompletionItem it;
                int score;
                if (!fuzzy_match(w, n, score, &it.hl)) continue;
                it.label = n;
                it.insert = n;
                out.push_back(it);
            }
            return out;
        };
    };
    Editor ed = fresh("");
    Completer tag;
    tag.trigger = tag_word_start;
    tag.items = source({"rust", "rust-lang", "research", "reading"});
    tag.lead = 1;
    ed.add_completer(tag);
    type(ed, "a #");
    CHECK(!ed.completing());  // a # alone isn't enough
    type(ed, "r");
    CHECK(ed.completing());
    key(ed, ImGuiKey_DownArrow);
    key(ed, ImGuiKey_Enter);
    CHECK_EQ(ed.text(), std::string("a #rust-lang"));
    CHECK(!ed.completing());
    // The word as typed, and it's the pick: enter breaks the line.
    type(ed, " #rust");
    CHECK(ed.completing());
    key(ed, ImGuiKey_Enter);
    CHECK_EQ(ed.text(), std::string("a #rust-lang #rust\n"));
    CHECK(!ed.completing());
    // Escape: away until the word's done.
    type(ed, "#re");
    CHECK(ed.completing());
    key(ed, ImGuiKey_Escape);
    CHECK(!ed.completing());
    type(ed, "a");
    CHECK(!ed.completing());
    type(ed, " #r");
    CHECK(ed.completing());
    key(ed, ImGuiKey_LeftArrow);  // moving off the word
    CHECK(!ed.completing());
    for (int i = 0; i < 30; i++) frame(ed);

    // Links: the ]] comes with it, or is stepped over if it's there.
    Editor e2 = fresh("");
    Completer link;
    link.trigger = md_link_word_start;
    link.items = source({"Plan", "work/Other"});
    link.min_chars = 0;
    link.close = "]]";
    e2.add_completer(link);
    type(e2, "see [[");
    CHECK(e2.completing());
    type(e2, "pl");
    key(e2, ImGuiKey_Enter);
    CHECK_EQ(e2.text(), std::string("see [[Plan]]"));
    Editor e3 = fresh("[[]]");
    e3.add_completer(link);
    key(e3, ImGuiKey_RightArrow);
    key(e3, ImGuiKey_RightArrow);
    CHECK(!e3.completing());  // the caret arriving isn't typing
    type(e3, "o");
    CHECK(e3.completing());
    key(e3, ImGuiKey_Enter);
    CHECK_EQ(e3.text(), std::string("[[work/Other]]"));
}

// What the editor completes to: every tag, ranked, and the notes to link to.
static void test_complete_sources() {
    TempDir tmp("delta_test_complete");
    Store store;
    store.set_root(tmp.path);
    NoteRef a, b, c, open;
    store.create_note("work", "a", "#rust #draft", a);
    store.create_note("work", "b", "#rust #rum", b);
    store.create_note("home", "c", "#Rust #ruby", c);
    store.create_note("work", "open", "#rx", open);
    TagIndex ix;
    ix.refresh(store.all_notes());
    auto labels = [](const std::vector<CompletionItem>& v) {
        std::string s;
        for (auto& it : v) s += (s.empty() ? "" : " ") + it.label;
        return s;
    };
    // The open note as it is in the editor, less the tag being written; a
    // used tag outranks a shorter one; "#r" is #rust, not #draft.
    std::vector<std::string> lines = {"#rustic #re"};
    auto tags = complete_tags(ix, open.path, lines, TextPos{0, 11}, "r");
    CHECK_EQ(labels(tags), std::string("#Rust #rum #ruby #rustic"));
    CHECK(tags.size() == 4 && tags[0].insert == "Rust" && tags[0].meta == "3" && tags[0].hl == std::vector<int>{1});
    CHECK(tags[3].meta == "1" && tags[3].dim == 0);
    // No note open: every note as it is on disk.
    CHECK_EQ(labels(complete_tags(ix, {}, {}, {}, "rx")), std::string("#rx"));
    CHECK(complete_tags(ix, open.path, lines, TextPos{0, 11}, "rx").empty());
    // At most 50.
    std::string many;
    for (int i = 0; i < 60; i++) many += "#t" + std::to_string(i) + " ";
    CHECK_EQ(complete_tags(TagIndex{}, open.path, {many}, TextPos{0, 0}, "t").size(), size_t(50));

    NoteIndex nx;
    nx.spaces["work"] = {"work", {{"alpha", "Alpha"}, {"beta", "Beta"}, {"me", "Me"}}};
    nx.spaces["home"] = {"Home", {{"alpha", "alpha"}, {"alps", "Alps"}}};
    // This space's by name, others' as "space/name", with the space dim.
    auto links = complete_links(nx, "work", "me", "al");
    CHECK_EQ(labels(links), std::string("Alpha Home/Alps Home/alpha"));
    CHECK(links[0].dim == 0 && links[0].hl == (std::vector<int>{0, 1}) && links[0].insert == "Alpha");
    CHECK(links[1].dim == 5 && links[1].hl == (std::vector<int>{5, 6}) && links[1].insert == "Home/Alps");
    // Among equals, this space's first, then by name; not to itself.
    CHECK_EQ(labels(complete_links(nx, "work", "me", "")), std::string("Alpha Beta Home/alpha Home/Alps"));
    CHECK_EQ(labels(complete_links(nx, "work", "", "m")), std::string("Me"));  // "hoMe/..." begins no word
    CHECK(complete_links(nx, "work", "me", "lp").empty());  // a match begins a word
    NoteIndex big;
    for (int i = 0; i < 60; i++) big.spaces["s"].notes["n" + std::to_string(i)] = "n" + std::to_string(i);
    big.spaces["s"].name = "s";
    CHECK_EQ(complete_links(big, "s", "", "").size(), size_t(50));
}

// Go to line: "42", "42:10", "+5" / "-5" from where you were.
static void test_line_target() {
    std::vector<std::string> lines = {"one", "two", "thr\xc3\xa9\xc3\xa9", "four"};
    TextPos to;
    auto at = [&](const std::string& q, int origin, int line, int col) {
        to = {};
        return parse_line_target(q, origin, lines, to) && to.line == line && to.col == col;
    };
    CHECK(at("2", 0, 1, 0));
    CHECK(at("2:3", 0, 1, 2));
    CHECK(at("2:", 0, 1, 0));
    CHECK(at("3:5", 0, 2, 5));   // columns count characters
    CHECK(at("3:99", 0, 2, 7));
    CHECK(at("+1", 1, 2, 0));
    CHECK(at("-5", 1, 0, 0));
    CHECK(at("99", 0, 3, 0));
    CHECK(at("-0", 2, 2, 0));
    for (const char* bad : {"", "0", "2x", ":3", "+", "2:x", " 2"}) CHECK(!parse_line_target(bad, 0, lines, to));
}

// A palette row that fills the query (a tag, in the # mode) keeps the
// palette open on the new query; a row that doesn't runs and closes it.
static void test_palette_fill() {
    Palette p;
    std::string built;
    bool ran = false;
    auto build = [&](const std::string& t, std::vector<PaletteItem>& out, int& total) {
        built = t;
        total = 1;
        PaletteItem it;
        it.label = "#rust";
        it.action = [&ran] { ran = true; };
        if (t == "#ru") it.fill = "#rust ";
        out.push_back(it);
    };
    auto run = [&](ImGuiKey k, std::function<void()> first = {}) {
        if (auto chosen = palette_frame(p, k, build, first)) chosen();
    };
    p.open("#ru");
    run(ImGuiKey_None);
    run(ImGuiKey_Enter);
    run(ImGuiKey_None);
    CHECK(p.is_open() && p.in_mode('#') && !ran);
    CHECK_EQ(built, std::string("#rust "));
    run(ImGuiKey_Enter);
    CHECK(ran && !p.is_open());
    for (int i = 0; i < 30; i++) run(ImGuiKey_None);

    // Asked by an enter (the tag rename's question): that enter isn't the answer.
    ran = false;
    PaletteItem yes;
    yes.label = "yes";
    yes.action = [&ran] { ran = true; };
    run(ImGuiKey_Enter, [&] { p.pick("sure? ", {yes}); });
    CHECK(!ran && p.is_open());
    run(ImGuiKey_None);  // (a release and a press of one key take a frame each)
    run(ImGuiKey_Enter);
    CHECK(ran);
    for (int i = 0; i < 30; i++) run(ImGuiKey_None);

    // Opened by a click outside it (ctrl+click on a #tag): that click doesn't
    // close it; the next one does.
    ImGuiIO& io = ImGui::GetIO();
    auto click = [&](std::function<void()> first = {}) {
        io.AddMousePosEvent(950, 750);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        run(ImGuiKey_None, first);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        run(ImGuiKey_None);
    };
    click([&] { p.open("#rust "); });
    CHECK(p.is_open());
    click();
    CHECK(!p.is_open());
    for (int i = 0; i < 30; i++) run(ImGuiKey_None);
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
    test_store();
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
    test_undo_history();
    test_link_problems();
    test_nav();
    test_spell_words();
    test_speller();
    test_spell_fix();
    test_spell_config();
    test_fonts();
    test_ui_voice();
    test_themes();
    test_manual();
    test_natural_sort();
    test_command_search();
    test_palette_modes();
    test_sandbox();
    test_config_set();
    test_font_loader();
    test_tags();
    test_completion();
    test_complete_sources();
    test_line_target();
    test_palette_fill();

    ImGui::DestroyContext();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
