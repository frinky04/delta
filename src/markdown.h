#pragma once
// Line-at-a-time markdown analysis. Every byte of a line gets a set of style
// flags; the editor draws the raw text with those styles (symbols stay
// visible, dimmed), the reader drops the S_Marker bytes.
#include <cstdint>
#include <string>
#include <vector>

enum : uint16_t {
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
};

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
};

struct LinkSpan {
    int begin, end;           // whole link incl. brackets
    std::string target;
    bool wiki;
};

bool md_is_fence(const std::string& s);
// One image in a line: ![alt](src), where alt may end in a size the way
// Obsidian writes it, "|300" (width) or "|300x200" (a box to fit in).
struct ImageRef {
    int begin = 0, end = 0;   // the whole ![...](...) in the line
    std::string alt;          // without the size
    std::string src;
    int w = 0, h = 0;         // size asked for, 0 = none
};
// Is this line nothing but images, one ![alt](src) or several separated by
// spaces (a row)? <angle brackets> around src and a "title" after it are
// allowed. `out` gets them in order.
bool md_image_row(const std::string& s, std::vector<ImageRef>* out = nullptr);
// The same, for callers that care about the first (or only) image.
bool md_image_line(const std::string& s, std::string* alt = nullptr, std::string* src = nullptr);
// An image as markdown, with its size in the alt text (w = 0: no size).
std::string md_image_text(const std::string& alt, const std::string& src, int w, int h = 0);
// The same image with a different width asked for (0: its own size).
std::string md_image_resized(const std::string& token, int w);
void md_analyze(const std::string& s, bool in_code, LineInfo& info, std::vector<uint16_t>& style,
                std::vector<LinkSpan>* links = nullptr);

// Reader view of one line: markers removed, bullets become "•", task boxes
// and quote bars become placeholders the renderer draws over. A task box
// placeholder is S_TaskBox without S_Marker (in the editor the "[ ]" always
// carries S_Marker), which is how the layout knows to size it as a box.
// `map[i]` is the source byte for output byte i.
void md_reader(const std::string& s, const LineInfo& info, const std::vector<uint16_t>& style,
               std::string& out, std::vector<uint16_t>& out_style, std::vector<int>& map);

float md_heading_scale(int level);
