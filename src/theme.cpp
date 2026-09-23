#include "theme.h"

#include <algorithm>
#include <cstdlib>

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

void build_theme(const Config& cfg, std::vector<ConfigIssue>& issues) {
    Theme t;
    // Monochrome: neutral greys only. The "accent" is simply the brightest
    // grey, used for the caret, markers and links.
    if (cfg.theme == "light") {
        t.dark = false;
        t.bg = hex(0xf4f4f4);
        t.surface = hex(0xffffff);
        t.border = hex(0xdedede);
        t.faint = hex(0xbdbdbd);
        t.dim = hex(0x8c8c8c);
        t.text = hex(0x2e2e2e);
        t.bright = hex(0x0a0a0a);
        t.accent = hex(0x111111);
        t.code = hex(0x4a4a4a);
        t.code_bg = hex(0xe9e9e9);
    } else {
        if (cfg.theme != "dark") issues.push_back({cfg.line_of("appearance.theme"), "unknown theme; use dark or light"});
        t.bg = hex(0x0c0c0c);
        t.surface = hex(0x151515);
        t.border = hex(0x242424);
        t.faint = hex(0x383838);
        t.dim = hex(0x6b6b6b);
        t.text = hex(0xbdbdbd);
        t.bright = hex(0xf0f0f0);
        t.accent = hex(0xe6e6e6);
        t.code = hex(0x9e9e9e);
        t.code_bg = hex(0x161616);
    }

    struct { const char* name; ImU32* dst; } slots[] = {
        {"background", &t.bg}, {"surface", &t.surface}, {"border", &t.border}, {"faint", &t.faint},
        {"dim", &t.dim}, {"text", &t.text}, {"bright", &t.bright}, {"accent", &t.accent},
        {"code", &t.code}, {"code_background", &t.code_bg},
    };
    for (auto& [name, value] : cfg.colors) {
        bool known = false;
        for (auto& s : slots) {
            if (name != s.name) continue;
            known = true;
            if (!parse_hex(value, *s.dst)) issues.push_back({cfg.line_of("colors." + name), "expected a colour like #1a2b3c"});
        }
        if (!known) issues.push_back({cfg.line_of("colors." + name), "unknown colour name"});
    }
    t.selection = col_alpha(t.accent, t.dark ? 0.17f : 0.14f);
    t.highlight = col_alpha(t.accent, t.dark ? 0.13f : 0.12f);
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
