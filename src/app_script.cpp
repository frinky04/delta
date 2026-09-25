// The --run script: steps passed on the command line, run as frames go by.
// For testing and scripting UI checks: command ids, "open:<note>" and
// "palette:<text>", and steps that type, press keys and move the mouse as
// a user would.
#include <cstdio>
#include <cstdlib>

#include "app.h"
#include "keys.h"

// Runs the steps whose time has come, up to the next that waits.
// A scripted key press (see "key:" below) is released the frame after.
void App::run_pending(double now) {
    ImGuiIO& io = ImGui::GetIO();
    if (release_key_ != ImGuiKey_None) {
        io.AddKeyEvent(release_key_, false);
        for (ImGuiKey m : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt}) io.AddKeyEvent(m, false);
        release_key_ = ImGuiKey_None;
    }
    // "wait:<seconds>" pauses the list, so later steps run once the page is up.
    if (!pending_run_.empty() && now >= run_after_) {
        auto run = std::move(pending_run_);
        pending_run_.clear();
        // The steps after the k-th wait `secs`.
        auto rest = [&](size_t k, double secs) {
            run_after_ = now + secs;
            pending_run_.assign(run.begin() + k + 1, run.end());
        };
        for (size_t k = 0; k < run.size(); k++) {
            const std::string& id = run[k];
            if (id.rfind("wait:", 0) == 0) {
                rest(k, std::atof(id.c_str() + 5));
                break;
            }
            if (id.rfind("open:", 0) == 0) { open_by_name(id.substr(5)); continue; }
            // "drop:<path>" drops a file at the caret; "fetch:<url>" pastes
            // an address as if it were on the clipboard.
            if (id.rfind("drop:", 0) == 0) { on_drop({id.substr(5)}, ImVec2(-1, -1)); continue; }
            if (id.rfind("fetch:", 0) == 0) { if (editing()) fetch_image(id.substr(6)); continue; }
            if (id.rfind("palette:", 0) == 0) { open_palette(id.substr(8)); continue; }
            if (id.rfind("find:", 0) == 0 || id.rfind("replace:", 0) == 0) {
                bool rep = id[0] == 'r';
                open_find(rep);
                std::snprintf(find_buf_, sizeof find_buf_, "%s", id.c_str() + (rep ? 8 : 5));
                continue;
            }
            // "key:<chord>" presses a key for a frame (then the list pauses a
            // moment so it's released before the next step); "type:<text>"
            // types text. For scripting UI checks.
            if (id.rfind("key:", 0) == 0) {
                Chord c;
                if (parse_chord(id.substr(4), c)) {
                    for (ImGuiKey m : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt}) io.AddKeyEvent(m, (c.mods & m) != 0);
                    io.AddKeyEvent(c.key, true);
                    release_key_ = c.key;
                }
                rest(k, 0.1);
                break;
            }
            // "mouse:<x>:<y>" moves the pointer (window pixels); "down" and
            // "up" press and let go of the left button, "rdown" and "rup"
            // the right.
            if (id.rfind("mouse:", 0) == 0 || id == "down" || id == "up" || id == "rdown" || id == "rup") {
                float mx = 0, my = 0;
                if (id[0] == 'm' && std::sscanf(id.c_str() + 6, "%f:%f", &mx, &my) == 2) io.AddMousePosEvent(mx, my);
                else if (id[0] == 'r') io.AddMouseButtonEvent(ImGuiMouseButton_Right, id == "rdown");
                else if (id[0] != 'm') io.AddMouseButtonEvent(ImGuiMouseButton_Left, id == "down");
                rest(k, 0.05);
                break;
            }
            if (id.rfind("type:", 0) == 0) {
                io.AddInputCharactersUTF8(id.c_str() + 5);
                rest(k, 0.1);
                break;
            }
            if (Command* c = find_command(id)) c->run();
        }
    }
}
