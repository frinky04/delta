#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "config.h"

Theme T;

static ImU32 hex(unsigned rgb) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
}

static bool parse_hex(const std::string& s, ImU32& out) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() != 6) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(h.c_str(), &end, 16);
    if (*end) return false;
    out = hex((unsigned)v);
    return true;
}

ImU32 col_alpha(ImU32 c, float a) {
    unsigned al = (c >> IM_COL32_A_SHIFT) & 0xFF;
    al = (unsigned)std::clamp(al * a, 0.0f, 255.0f);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 col_mix(ImU32 a, ImU32 b, float t) {
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

unsigned col_rgb(ImU32 c) {
    return (((c >> IM_COL32_R_SHIFT) & 0xFF) << 16) | (((c >> IM_COL32_G_SHIFT) & 0xFF) << 8) | ((c >> IM_COL32_B_SHIFT) & 0xFF);
}

const char* const kColourSlots[12] = {"background", "surface", "border", "faint", "dim", "text",
                                      "bright", "accent", "code", "code_background", "error", "spelling"};

static float luminance(unsigned rgb) {
    return (0.2126f * ((rgb >> 16) & 0xFF) + 0.7152f * ((rgb >> 8) & 0xFF) + 0.0722f * (rgb & 0xFF)) / 255.0f;
}

bool ThemeDef::dark() const { return luminance(colours[0]) < 0.5f; }

static unsigned mix(unsigned a, unsigned b, float t) {
    unsigned out = 0;
    for (int shift : {16, 8, 0}) {
        float x = (float)((a >> shift) & 0xFF), y = (float)((b >> shift) & 0xFF);
        out |= (unsigned)std::lround(x + (y - x) * t) << shift;
    }
    return out;
}

// WCAG contrast ratio, 1 to 21.
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

// `c`, moved toward `to` just far enough to stand `ratio` apart from `bg`.
static unsigned hold(unsigned c, unsigned bg, unsigned to, float ratio) {
    for (float t = 0; t < 1 && contrast(c, bg) < ratio; t += 0.05f) c = mix(c, to, 0.05f);
    return c;
}

// A monkeytype theme, from its own colours: `bg` behind everything,
// `main` its one colour, `sub` for what's secondary, `sub_alt` a second
// background, `text` what you type, and `error`. Delta needs twelve, so
// the rest are mixed from those the way delta's own themes step between
// them. `main` stays the accent alone: headings get brighter, not coloured.
// Secondary text is still text, so `sub` is pushed toward `text` until it
// reads at 4.5:1; the caret and the palette's pick are marks, so `main` is
// pushed out to 3:1.
static ThemeDef port(const char* name, unsigned bg, unsigned main, unsigned sub, unsigned sub_alt, unsigned text,
                     unsigned error) {
    unsigned far = contrast(0xffffff, bg) > contrast(0x000000, bg) ? 0xffffff : 0x000000;
    sub = hold(sub, bg, text, 4.5f);
    main = hold(main, bg, far, 3.0f);
    return {name,
            {bg, sub_alt, mix(bg, sub, 0.25f), mix(bg, sub, 0.5f), sub, text, mix(text, far, 0.5f), main,
             mix(text, sub, 0.35f), mix(bg, text, 0.06f), error, mix(error, sub, 0.25f)}};
}

const std::vector<ThemeDef>& all_themes() {
    static const std::vector<ThemeDef> themes = {
        // Neutral greys carry everything you've written; hierarchy comes
        // from brightness and weight. The one colour, `accent`, marks only
        // where you are acting: the caret, the selection, the palette's
        // pick, the current line number, the unsaved dot and the echo of an
        // edit. Never content, never text (except dark text on a solid
        // accent bar). `error` is the same kind of signal for problems:
        // marks only, never the message.
        {"dark",
         {0x0c0c0c, 0x151515, 0x242424, 0x383838, 0x7f7f7f, 0xbdbdbd, 0xf0f0f0,
          0xe0851e,  // warm orange, after Krita's dark theme
          0x9e9e9e, 0x161616,
          0xd0564a,    // muted brick red
          0xac5d55}},  // the same, nearer grey
        {"light",
         {0xf4f4f4, 0xffffff, 0xdedede, 0xbdbdbd, 0x696969, 0x2e2e2e, 0x0a0a0a,
          0xc96f0e,  // deeper, so it holds up on light grey
          0x4a4a4a, 0xe9e9e9, 0xb8412f, 0xa95b50}},

        // From monkeytype.
        port("serika_dark", 0x323437, 0xe2b714, 0x646669, 0x2c2e31, 0xd1d0c5, 0xca4754),
        port("serika", 0xe1e1e3, 0xe2b714, 0xaaaeb3, 0xd1d3d8, 0x323437, 0xda3333),
        port("carbon", 0x313131, 0xf66e0d, 0x616161, 0x2b2b2b, 0xf5e6c8, 0xe72d2d),
        port("8008", 0x333a45, 0xf44c7f, 0x939eae, 0x2e343d, 0xe9ecf0, 0xda3333),
        port("olivia", 0x1c1b1d, 0xdeaf9d, 0x4e3e3e, 0x262426, 0xf2efed, 0xbf616a),
        port("bento", 0x2d394d, 0xff7a90, 0x4a768d, 0x263041, 0xfffaf8, 0xee2a3a),
        port("moonlight", 0x191f28, 0xc69f68, 0x4b5975, 0x141a22, 0xccccb5, 0xb81b2c),

        // Editor themes, by way of their monkeytype ports.
        port("nord", 0x2e3440, 0x88c0d0, 0x616e88, 0x3b4252, 0xeceff4, 0xbf616a),
        port("dracula", 0x282a36, 0xbd93f9, 0x6272a4, 0x21222c, 0xf8f8f2, 0xff5555),
        port("gruvbox_dark", 0x282828, 0xd79921, 0x665c54, 0x1d2021, 0xebdbb2, 0xfb4934),
        port("gruvbox_light", 0xfbf1c7, 0x689d6a, 0xa89984, 0xebdbb2, 0x3c3836, 0xcc241d),
        port("catppuccin", 0x1e1e2e, 0xcba6f7, 0x7f849c, 0x181825, 0xcdd6f4, 0xf38ba8),
        port("rose_pine", 0x191724, 0x9ccfd8, 0x6e6a86, 0x1f1d2e, 0xe0def4, 0xeb6f92),
        port("rose_pine_dawn", 0xfaf4ed, 0x56949f, 0x9893a5, 0xf2e9e1, 0x575279, 0xb4637a),
        port("tokyo_night", 0x1a1b26, 0x7aa2f7, 0x565f89, 0x16161e, 0xc0caf5, 0xf7768e),
        port("everforest", 0x2d353b, 0xa7c080, 0x859289, 0x272e33, 0xd3c6aa, 0xe67e80),
    };
    return themes;
}

const ThemeDef* find_theme(const std::string& name) {
    for (auto& t : all_themes())
        if (t.name == name) return &t;
    return nullptr;
}

std::string theme_colour(const std::string& theme, const std::string& slot) {
    const ThemeDef* t = find_theme(theme);
    if (!t) t = &all_themes().front();
    for (int i = 0; i < 12; i++)
        if (slot == kColourSlots[i]) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "#%06x", t->colours[i]);
            return buf;
        }
    return {};
}

void build_theme(const Config& cfg, const std::string* theme) {
    const ThemeDef* def = find_theme(theme ? *theme : cfg.theme);
    if (!def) def = &all_themes().front();  // the config reports it
    Theme t;
    ImU32* slots[12] = {&t.bg, &t.surface, &t.border, &t.faint, &t.dim, &t.text,
                        &t.bright, &t.accent, &t.code, &t.code_bg, &t.error, &t.spelling};
    for (int i = 0; i < 12; i++) *slots[i] = hex(def->colours[i]);
    // Names and values were checked when the config was read.
    for (auto& [name, value] : cfg.colors)
        for (int i = 0; i < 12; i++)
            if (name == kColourSlots[i]) parse_hex(value, *slots[i]);
    t.dark = luminance(col_rgb(t.bg)) < 0.5f;
    t.selection = col_alpha(t.accent, t.dark ? 0.26f : 0.22f);  // translucent: it covers text
    t.highlight = col_alpha(t.bright, t.dark ? 0.13f : 0.12f);  // ==marked== text is content: neutral
    T = t;
}

void apply_imgui_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.WindowBorderSize = 0;
    s.WindowRounding = 0;
    s.FrameBorderSize = 0;
    s.FramePadding = ImVec2(0, 0);
    s.ItemSpacing = ImVec2(0, 0);
    s.PopupBorderSize = 0;

    ImVec4* c = s.Colors;
    auto v = [](ImU32 x) { return ImGui::ColorConvertU32ToFloat4(x); };
    c[ImGuiCol_Text] = v(T.text);
    c[ImGuiCol_TextDisabled] = v(T.dim);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(T.surface);
    c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg] = v(T.selection);
    c[ImGuiCol_InputTextCursor] = v(T.accent);
    c[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Border] = v(T.border);
}
