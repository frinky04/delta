#pragma once
// Line-at-a-time markdown analysis. Every byte of a line gets a set of style
// flags; the editor draws the raw text with those styles (symbols stay
// visible, dimmed), the reader drops the S_Marker bytes.
#include <cstdint>
#include <string>
#include <vector>

// Per-byte style flags.
using Style = uint32_t;

enum : Style {
    S_Marker    = 1 << 0,   // syntax characters (#, **, `, [, ](url) ...)
    S_Bold      = 1 << 1,
    S_Italic    = 1 << 2,
    S_Code      = 1 << 3,   // inline code
    S_Strike    = 1 << 4,
    S_Link      = 1 << 5,   // link text or bare url
    S_Url       = 1 << 6,   // (url) part / wiki target before |
    S_Heading   = 1 << 7,
    S_ListMark  = 1 << 8,   // bullet or number
    S_Quote     = 1 << 9,   // quoted text
    S_QuoteMark = 1 << 10,  // the > characters
    S_TaskBox   = 1 << 11,  // [ ] / [x]
    S_TaskDone  = 1 << 12,
    S_CodeBlock = 1 << 13,  // fenced code content or the fence itself
    S_Highlight = 1 << 14,  // ==text==
    S_WikiLink  = 1 << 15,  // [[note]]
    S_Tag       = 1 << 16,  // #tag, its # included (not a marker: the reader keeps it)
    S_TableBar  = 1 << 17,  // a table's | (and its whole delimiter row)
};

// A line's part in a table (GFM): a header row, the delimiter row under it
// (| --- | :-: |), then body rows up to the first line without a |. Table
// rows are still paragraphs to everything else; the reader draws them as a
// grid.
enum class TableRole : uint8_t { None, Header, Delim, Body };
enum class Align : uint8_t { None, Left, Center, Right };

// Image: a line holding nothing but ![alt](src). It's shown as the image
// itself, an object the caret steps over, rather than as text.
enum class LineKind : uint8_t { Blank, Para, Heading, Fence, CodeBlock, Rule, Image };

struct LineInfo {
    LineKind kind = LineKind::Para;
    int level = 0;            // heading level 1..6
    bool quote = false;
    bool list = false;
    bool ordered = false;
    bool task = false;
    bool done = false;
    int marker_begin = 0;     // start of bullet/number (after quote + indent)
    int content_start = 0;    // first byte after all block markers
    TableRole table = TableRole::None;
};

// A link, or a #tag (following one lists the notes that have it).
struct LinkSpan {
    int begin, end;           // whole link incl. brackets (a tag's # included)
    std::string target;       // a tag's name, without the #
    bool wiki;
    bool tag = false;
};

bool md_is_fence(const std::string& s);
// What a word is made of: letters, digits and anything non-ASCII.
bool md_word_char(unsigned char c);
// What a tag's name is made of: letters, digits, _ - and /.
bool md_tag_char(unsigned char c);
// The tag whose name starts at s[at] (just after a #): where the name ends,
// or `at` if there's no tag there. A tag stands apart from the word before
// it and isn't only digits ("#12" is an issue number).
int md_tag_end(const std::string& s, int at);
// The [[link]] being written at `col` ("[[pl|" or "[[pl|]]"): where its
// target begins, or -1 if there's none there. `st` is the line's styles.
int md_link_word_start(const std::string& s, int col, const std::vector<Style>& st);
// One image in a line: ![alt](src), where alt may end in a size the way
// Obsidian writes it, "|300" (width) or "|300x200" (a box to fit in).
struct ImageRef {
    int begin = 0, end = 0;   // the whole ![...](...) in the line
    std::string alt;          // without the size
    std::string src;
    int src_at = 0;           // where src is written in the line
    int w = 0, h = 0;         // size asked for, 0 = none
};
// Is this line nothing but images, one ![alt](src) or several separated by
// spaces (a row)? <angle brackets> around src and a "title" after it are
// allowed. `out` gets them in order.
bool md_image_row(const std::string& s, std::vector<ImageRef>* out = nullptr);
// The same, for callers that care about the first (or only) image.
bool md_image_line(const std::string& s, std::string* alt = nullptr, std::string* src = nullptr);
// The one image starting at s[at] ("!["), wherever it is in the line.
bool md_image_at(const std::string& s, size_t at, ImageRef& ref);
// An image as markdown, with its size in the alt text (w = 0: no size).
std::string md_image_text(const std::string& alt, const std::string& src, int w, int h = 0);
// The same image with a different width asked for (0: its own size).
std::string md_image_resized(const std::string& token, int w);
void md_analyze(const std::string& s, bool in_code, LineInfo& info, std::vector<Style>& style,
                std::vector<LinkSpan>* links = nullptr);
// Which lines are fenced code, the fences included.
std::vector<bool> md_code_lines(const std::vector<std::string>& lines);
// Every link, url and #tag outside fenced code, with its line, in order.
struct LineLink { int line; LinkSpan link; };
std::vector<LineLink> md_links(const std::vector<std::string>& lines);

// Tables. A row's cells: where each one's text is, spaces around it left
// out. The | at either end is optional, and \| is a | inside a cell (in
// code spans too, as in GFM).
struct TableCell { int begin, end; };
std::vector<TableCell> md_table_cells(const std::string& s);
// Is this a delimiter row? `align` gets each column's alignment.
bool md_table_delim(const std::string& s, std::vector<Align>* align = nullptr);
// Every line's part in a table, fenced code left out.
std::vector<TableRole> md_table_roles(const std::vector<std::string>& lines);
// md_analyze for a table row: each cell's inline markdown on its own, the
// |s marked S_Marker | S_TableBar (a delimiter row is all bar), a header's
// text bold.
void md_table_row(const std::string& s, TableRole role, LineInfo& info, std::vector<Style>& style,
                  std::vector<LinkSpan>* links = nullptr);
// A table row's reader text: its cells' text one after another, markers
// dropped. `bounds` gets where each cell starts, and the end.
void md_table_reader(const std::string& s, const std::vector<Style>& style, std::string& out,
                     std::vector<Style>& out_style, std::vector<int>& map, std::vector<int>& bounds);

// Reader view of one line: markers removed, bullets become "•", task boxes
// and quote bars become placeholders the renderer draws over. A task box
// placeholder is S_TaskBox without S_Marker (in the editor the "[ ]" always
// carries S_Marker), which is how the layout knows to size it as a box.
// `map[i]` is the source byte for output byte i.
void md_reader(const std::string& s, const LineInfo& info, const std::vector<Style>& style,
               std::string& out, std::vector<Style>& out_style, std::vector<int>& map);

float md_heading_scale(int level);
