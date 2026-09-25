// delta: spaces-based markdown notes.
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"  // ConfigNavWindowingKey*
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform.h"
#include "theme.h"
#include "util.h"

#include "stb_image_write.h"  // implemented in images.cpp

// Frames still owed after an input event: ImGui needs a couple of frames to
// settle hover/active state, then the loop can go back to sleeping.
static int g_wake = 3;
static App* g_app = nullptr;

static void wake() { g_wake = 3; }

// Seconds per refresh of the monitor the window is mostly on (0 if unknown).
static float refresh_interval(GLFWwindow* w);

// One frame: input to ImGui, the app, and drawing into the back buffer (the
// caller swaps). Also run from the resize callbacks: while you drag a window
// edge, Windows holds the thread in its own loop inside glfwPollEvents, so
// without this nothing would be drawn until you let go.
static GLFWwindow* g_window = nullptr;
static FILE* g_frame_log = nullptr;  // --frame-log: one line per frame, for pacing checks
static bool g_in_frame = false;
static bool g_ready = false;

static bool render_frame(bool slept) {
    if (g_in_frame || !g_app || !g_ready) return false;
    g_in_frame = true;
    ImGuiIO& io = ImGui::GetIO();
    anim::active = false;
    g_app->apply_fonts();  // a saved config may have picked others
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    // Animations advance one display refresh per frame (see anim::tick).
    static float interval = 0;
    static int since_check = 1000;
    if (++since_check > 60) { interval = refresh_interval(g_window); since_check = 0; }
    anim::tick(io.DeltaTime, slept, interval);
    g_app->frame();
    ImGui::Render();

    int w, h;
    glfwGetFramebufferSize(g_window, &w, &h);
    glViewport(0, 0, w, h);
    ImVec4 bg = ImGui::ColorConvertU32ToFloat4(g_app->background());
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (g_frame_log) std::fprintf(g_frame_log, "%.4f dt=%.4f step=%.4f slept=%d active=%d\n", glfwGetTime(), io.DeltaTime, anim::frame_step, slept, anim::active);
    g_in_frame = false;
    return true;
}

// The window is being resized (or needs repainting): draw at the new size
// straight away.
static void on_refresh(GLFWwindow* w) {
    int fw, fh;
    glfwGetFramebufferSize(w, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;
    if (render_frame(false)) glfwSwapBuffers(w);
}
static void on_key(GLFWwindow*, int, int, int, int) { wake(); }
static void on_char(GLFWwindow*, unsigned) { wake(); }
static void on_mouse(GLFWwindow*, int, int, int) { wake(); }
static void on_move(GLFWwindow*, double, double) { wake(); }
static void on_scroll(GLFWwindow*, double, double) { wake(); }
static void on_enter(GLFWwindow*, int) { wake(); }
static void on_size(GLFWwindow* w, int, int) {
    wake();
    on_refresh(w);
}
static void on_focus(GLFWwindow*, int focused) {
    wake();
    if (g_app) g_app->on_focus(focused != 0);
}
static void on_drop(GLFWwindow* w, int count, const char** paths) {
    wake();
    if (!g_app) return;
    double x = 0, y = 0;
    glfwGetCursorPos(w, &x, &y);
    g_app->on_drop(std::vector<std::string>(paths, paths + count), ImVec2((float)x, (float)y));
}
static void on_scale(GLFWwindow*, float x, float) {
    wake();
    if (g_app) g_app->set_dpi(x);
}

// Animations step by this much per frame; see anim::tick.
static float refresh_interval(GLFWwindow* w) {
    int wx, wy, ww, wh;
    glfwGetWindowPos(w, &wx, &wy);
    glfwGetWindowSize(w, &ww, &wh);
    int cx = wx + ww / 2, cy = wy + wh / 2;
    GLFWmonitor* best = glfwGetPrimaryMonitor();
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    for (int i = 0; i < count; i++) {
        int mx, my;
        glfwGetMonitorPos(mons[i], &mx, &my);
        const GLFWvidmode* m = glfwGetVideoMode(mons[i]);
        if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height) best = mons[i];
    }
    const GLFWvidmode* m = best ? glfwGetVideoMode(best) : nullptr;
    return m && m->refreshRate > 0 ? 1.0f / (float)m->refreshRate : 0.0f;
}

int main(int argc, char** argv) {
    AppOptions opts;
    std::string shot;
    int shot_w = 0, shot_h = 0;
    double shot_delay = 1.0;  // seconds after the first frame
    // --sandbox [dir]: a config and notes of its own, as on a first launch.
    // Without a dir it's a temporary one, removed on exit; a dir is kept, so
    // the next run with it is a second launch.
    fs::path sandbox_dir;
    bool keep_sandbox = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") opts.config_path = u8path(next());
        else if (a == "--sandbox") {
            opts.sandbox = true;
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
                sandbox_dir = u8path(next());
                keep_sandbox = true;
            }
        }
        else if (a == "--run") {
            std::string list = next();
            size_t start = 0;
            while (start <= list.size()) {
                size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                if (comma > start) opts.run.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
        } else if (a == "--screenshot") shot = next();
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &shot_w, &shot_h);
        else if (a == "--shot-delay") shot_delay = std::atof(next().c_str());
        else if (a == "--frame-log") g_frame_log = std::fopen(next().c_str(), "w");
    }
    if (opts.sandbox) {
        if (sandbox_dir.empty()) {
            auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            sandbox_dir = fs::temp_directory_path() / ("delta-sandbox-" + std::to_string(stamp));
        }
        std::string err;
        opts.config_path = make_sandbox(sandbox_dir, &err);
        if (opts.config_path.empty()) {
            std::fprintf(stderr, "delta: can't make the sandbox: %s\n", err.c_str());
            return 1;
        }
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown after the first frame: no white flash
    glfwWindowHint(GLFW_SAMPLES, 0);
    if (!shot.empty() || g_frame_log) {
        // Screenshot and logging runs must never steal focus (or keystrokes)
        // from whatever the user is doing.
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    }
    GLFWwindow* window = glfwCreateWindow(shot_w ? shot_w : 1100, shot_h ? shot_h : 820, "delta", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Installed before ImGui so its backend chains to them.
    glfwSetKeyCallback(window, on_key);
    glfwSetCharCallback(window, on_char);
    glfwSetMouseButtonCallback(window, on_mouse);
    glfwSetCursorPosCallback(window, on_move);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetCursorEnterCallback(window, on_enter);
    glfwSetWindowFocusCallback(window, on_focus);
    glfwSetWindowSizeCallback(window, on_size);
    glfwSetWindowRefreshCallback(window, on_refresh);
    glfwSetDropCallback(window, on_drop);
    glfwSetWindowContentScaleCallback(window, on_scale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigInputTextCursorBlink = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // ctrl+tab is delta's note switcher, not ImGui's window switcher.
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    App app;
    g_app = &app;
    g_window = window;
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    app.set_dpi(sx);
    if (!app.init(window, opts)) return 1;
    g_ready = true;  // resize callbacks may draw from here on

    bool shown = false;
    int frames = 0;
    while (!glfwWindowShouldClose(window) && !app.should_quit()) {
        double now = glfwGetTime();
        bool slept = false;
        if (g_wake > 0 || anim::active || !shown || !shot.empty()) {
            glfwPollEvents();
        } else {
            double wait = app.next_wake(now) - now;
            glfwWaitEventsTimeout(wait > 0.001 ? wait : 0.001);
            slept = true;
        }
        if (g_wake > 0) g_wake--;
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) { glfwWaitEvents(); continue; }

        render_frame(slept);

        // Capture once animations have had real time to settle.
        static double first_frame = glfwGetTime();
        if (!shot.empty() && ++frames >= 3 && glfwGetTime() - first_frame > shot_delay) {
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            std::vector<unsigned char> px((size_t)w * h * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            stbi_flip_vertically_on_write(1);
            stbi_write_png(shot.c_str(), w, h, 4, px.data(), w * 4);
            break;
        }

        glfwSwapBuffers(window);
        if (!shown) {
            glfwShowWindow(window);
            shown = true;
        }
    }

    g_ready = false;
    if (g_frame_log) std::fclose(g_frame_log);
    g_frame_log = nullptr;
    app.shutdown();
    g_app = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (opts.sandbox && !keep_sandbox) {
        std::error_code ec;
        fs::remove_all(sandbox_dir, ec);
    }
    return 0;
}
