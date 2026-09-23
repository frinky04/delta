#include "keys.h"

#include <sstream>

#include "util.h"

struct KeyName { const char* name; ImGuiKey key; };

static const KeyName kNames[] = {
    {"up", ImGuiKey_UpArrow}, {"down", ImGuiKey_DownArrow}, {"left", ImGuiKey_LeftArrow}, {"right", ImGuiKey_RightArrow},
    {"enter", ImGuiKey_Enter}, {"escape", ImGuiKey_Escape}, {"esc", ImGuiKey_Escape}, {"tab", ImGuiKey_Tab},
    {"space", ImGuiKey_Space}, {"backspace", ImGuiKey_Backspace}, {"delete", ImGuiKey_Delete}, {"insert", ImGuiKey_Insert},
    {"home", ImGuiKey_Home}, {"end", ImGuiKey_End}, {"pageup", ImGuiKey_PageUp}, {"pagedown", ImGuiKey_PageDown},
    {"=", ImGuiKey_Equal}, {"-", ImGuiKey_Minus}, {",", ImGuiKey_Comma}, {".", ImGuiKey_Period},
    {"/", ImGuiKey_Slash}, {";", ImGuiKey_Semicolon}, {"'", ImGuiKey_Apostrophe}, {"[", ImGuiKey_LeftBracket},
    {"]", ImGuiKey_RightBracket}, {"\\", ImGuiKey_Backslash}, {"`", ImGuiKey_GraveAccent},
    {"numpad+", ImGuiKey_KeypadAdd}, {"numpad-", ImGuiKey_KeypadSubtract}, {"numpadenter", ImGuiKey_KeypadEnter},
};

static ImGuiKey key_from_name(const std::string& n) {
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return (ImGuiKey)(ImGuiKey_A + (n[0] - 'a'));
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return (ImGuiKey)(ImGuiKey_0 + (n[0] - '0'));
    if (n.size() >= 2 && n[0] == 'f') {
        int f = std::atoi(n.c_str() + 1);
        if (f >= 1 && f <= 12) return (ImGuiKey)(ImGuiKey_F1 + (f - 1));
    }
    for (auto& k : kNames) if (n == k.name) return k.key;
    return ImGuiKey_None;
}

static std::string name_from_key(ImGuiKey k) {
    if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return std::string(1, (char)('a' + (k - ImGuiKey_A)));
    if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return std::string(1, (char)('0' + (k - ImGuiKey_0)));
    if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return "f" + std::to_string(1 + (k - ImGuiKey_F1));
    for (auto& n : kNames) if (n.key == k) return n.name;
    return "?";
}

bool parse_chord(const std::string& spec, Chord& out) {
    std::string s = lower(trim(spec));
    if (s.empty()) return false;
    Chord c;
    // Split on '+', but a trailing "+" / "numpad+" belongs to the key.
    size_t start = 0;
    while (true) {
        size_t plus = s.find('+', start);
        if (plus == std::string::npos || plus == s.size() - 1) {
            std::string key = s.substr(start);
            c.key = key_from_name(key);
            if (c.key == ImGuiKey_None && key == "+") c.key = ImGuiKey_Equal;
            break;
        }
        std::string mod = s.substr(start, plus - start);
        if (mod == "numpad") { c.key = ImGuiKey_KeypadAdd; break; }
        if (mod == "ctrl" || mod == "control" || mod == "cmd") c.mods |= ImGuiMod_Ctrl;
        else if (mod == "shift") c.mods |= ImGuiMod_Shift;
        else if (mod == "alt" || mod == "option") c.mods |= ImGuiMod_Alt;
        else if (mod == "super" || mod == "win" || mod == "meta") c.mods |= ImGuiMod_Super;
        else return false;
        start = plus + 1;
    }
    if (c.key == ImGuiKey_None) return false;
    out = c;
    return true;
}

std::vector<Chord> parse_chords(const std::string& s, std::vector<std::string>* bad) {
    std::vector<Chord> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) {
        Chord c;
        if (parse_chord(tok, c)) out.push_back(c);
        else if (bad) bad->push_back(tok);
    }
    return out;
}

std::string chord_str(const Chord& c) {
    std::string s;
    if (c.mods & ImGuiMod_Ctrl) s += "ctrl+";
    if (c.mods & ImGuiMod_Shift) s += "shift+";
    if (c.mods & ImGuiMod_Alt) s += "alt+";
    if (c.mods & ImGuiMod_Super) s += "super+";
    return s + name_from_key(c.key);
}

bool chord_pressed(const Chord& c, bool repeat) {
    if (c.key == ImGuiKey_None) return false;
    int mods = ImGui::GetIO().KeyMods & (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt | ImGuiMod_Super);
    return mods == c.mods && ImGui::IsKeyPressed(c.key, repeat);
}
