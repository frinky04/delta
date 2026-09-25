#pragma once
// Small shared helpers: UTF-8 stepping, animation easing, string bits.
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

namespace utf8 {

inline bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

inline unsigned decode(const char* s, const char* e, int* len) {
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (s + n > e) n = 1;
    unsigned cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int i = 1; i < n; i++) {
        if (!is_cont((unsigned char)s[i])) { n = 1; cp = 0xFFFD; break; }
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *len = n;
    return cp;
}

inline int next(const std::string& s, int i) {
    if (i >= (int)s.size()) return (int)s.size();
    i++;
    while (i < (int)s.size() && is_cont((unsigned char)s[i])) i++;
    return i;
}

inline int prev(const std::string& s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && is_cont((unsigned char)s[i])) i--;
    return i;
}

inline void append(std::string& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

} // namespace utf8

// Animations are purely visual: they chase a target that input has already
// set. `active` is raised whenever something is still moving so the main loop
// knows to keep rendering instead of sleeping.
// ImGui sizes a font by its line box (ascent + descent); CSS sizes by the em.
// For Noto Sans (and Noto Sans Mono) the box is 1.362 em, so "16px" text in
// the CSS sense needs ImGui size 16 * kEm. Config values use CSS meaning.
constexpr float kEm = 1.362f;

namespace anim {

inline bool enabled = true;
inline float speed = 1.0f;
inline bool active = false;

// Animation time advances in whole display frames, not measured time.
//
// Measured frame times are misleading here: delta sleeps between inputs, so
// the first frame after a pause "took" seconds, and the frames right after
// waking come in a burst (the driver refilling its queue) yet are still shown
// one per refresh. Stepping by measured time made every tap jump most of the
// way on its first frame, then stall for a few. Stepping by the refresh
// interval moves every displayed frame by the same amount.
//
// tick() runs once per frame (from the main loop): one interval per frame,
// two if a frame was genuinely missed. `clock` is animation time, for
// transitions timed from a start point.
inline double clock = 0;
inline float frame_step = 0;  // this frame's animation dt; 0 until tick() runs

inline void tick(float dt, bool woke, float interval) {
    float s;
    if (interval > 0) {
        float n = woke ? 1.0f : std::round(dt / interval);
        s = (n < 1 ? 1 : n > 2 ? 2 : n) * interval;
    } else {
        s = dt < 1.0f / 30 ? dt : 1.0f / 30;  // no display rate known: cap it
    }
    frame_step = s;
    clock += s;
}

inline float step(float dt) {
    if (frame_step > 0) return frame_step;
    return dt < 1.0f / 30 ? dt : 1.0f / 30;
}

inline float approach(float cur, float target, float rate, float dt, float eps = 0.25f) {
    float d = target - cur;
    if (!enabled || std::fabs(d) <= eps) return target;
    active = true;
    return target - d * std::exp(-rate * speed * step(dt));
}

// 0..1 progress of a tween that started at `start`.
inline float progress(double start, double now, float duration) {
    if (!enabled) return 1.0f;
    float t = (float)((now - start) / (duration / speed));
    if (t < 1.0f) active = true;
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

inline float ease_out(float t) { float u = 1 - t; return 1 - u * u * u; }

} // namespace anim

inline std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return std::string(s.substr(a, b - a));
}

inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// A file's text as lines, without their line endings ('\r' dropped), and back.
inline std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out(1);
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

inline std::string join_lines(const std::vector<std::string>& lines, std::string_view eol = "\n") {
    size_t total = 0;
    for (auto& l : lines) total += l.size() + eol.size();
    std::string out;
    out.reserve(total);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += eol;
        out += lines[i];
    }
    return out;
}

inline bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

// Name order as people read it: case aside, and runs of digits by value,
// so "note 2" comes before "note 10".
inline bool natural_less(std::string_view a, std::string_view b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[j];
        if (std::isdigit(x) && std::isdigit(y)) {
            size_t i0 = i, j0 = j;
            while (i0 < a.size() && a[i0] == '0') i0++;
            while (j0 < b.size() && b[j0] == '0') j0++;
            size_t i1 = i0, j1 = j0;
            while (i1 < a.size() && std::isdigit((unsigned char)a[i1])) i1++;
            while (j1 < b.size() && std::isdigit((unsigned char)b[j1])) j1++;
            if (i1 - i0 != j1 - j0) return i1 - i0 < j1 - j0;  // more digits, bigger number
            if (int c = a.substr(i0, i1 - i0).compare(b.substr(j0, j1 - j0)); c != 0) return c < 0;
            i = i1, j = j1;
            continue;
        }
        x = (unsigned char)std::tolower(x), y = (unsigned char)std::tolower(y);
        if (x != y) return x < y;
        i++, j++;
    }
    return a.size() - i < b.size() - j;
}

// Text from elsewhere (the OS's "Access is denied.") in delta's voice:
// lower case, no full stop. An acronym at the start ("OS ...") keeps its case.
inline std::string plain_msg(std::string_view s) {
    std::string t = trim(s);
    while (!t.empty() && t.back() == '.') t.pop_back();
    if (t.size() > 1 && std::isupper((unsigned char)t[0]) && !std::isupper((unsigned char)t[1]))
        t[0] = (char)std::tolower((unsigned char)t[0]);
    return t;
}
