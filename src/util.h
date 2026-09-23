#pragma once
// Small shared helpers: UTF-8 stepping, animation easing, string bits.
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
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

// The most time one frame may count towards an animation. delta sleeps
// between inputs, so the first frame after a pause can be seconds after the
// last one; uncapped, that frame would finish every animation at once, and
// motion would seem to randomly not happen whenever you'd paused first.
inline float step(float dt) { return dt < 1.0f / 30 ? dt : 1.0f / 30; }

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

inline bool iequals(const std::string& a, const std::string& b) { return lower(a) == lower(b); }
