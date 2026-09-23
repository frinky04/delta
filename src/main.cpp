// delta: spaces-based markdown notes.
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform.h"
#include "theme.h"
#include "util.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

// Frames still owed after an input event: ImGui needs a couple of frames to
// settle hover/active state, then the loop can go back to sleeping.
static int g_wake = 3;
static App* g_app = nullptr;

static void wake() { g_wake = 3; }
static void on_key(GLFWwindow*, int, int, int, int) { wake(); }
static void on_char(GLFWwindow*, unsigned) { wake(); }
static void on_mouse(GLFWwindow*, int, int, int) { wake(); }
static void on_move(GLFWwindow*, double, double) { wake(); }
static void on_scroll(GLFWwindow*, double, double) { wake(); }
static void on_enter(GLFWwindow*, int) { wake(); }
static void on_size(GLFWwindow*, int, int) { wake(); }
static void on_focus(GLFWwindow*, int focused) {
    wake();
    if (g_app) g_app->on_focus(focused != 0);
}
static void on_scale(GLFWwindow*, float x, float) {
    wake();
    if (g_app) g_app->set_dpi(x);
}

int main(int argc, char** argv) {
    AppOptions opts;
    std::string shot;
    int shot_w = 0, shot_h = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--config") opts.config_path = u8path(next());
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
    }

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown after the first frame: no white flash
    glfwWindowHint(GLFW_SAMPLES, 0);
    if (!shot.empty()) {
        // Screenshot runs must never steal focus (or keystrokes) from
        // whatever the user is doing.
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
    glfwSetWindowContentScaleCallback(window, on_scale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigInputTextCursorBlink = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    App app;
    g_app = &app;
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    app.set_dpi(sx);
    if (!app.init(window, opts)) return 1;

    bool shown = false;
    int frames = 0;
    while (!glfwWindowShouldClose(window) && !app.should_quit()) {
        double now = glfwGetTime();
        if (g_wake > 0 || anim::active || !shown || !shot.empty()) {
            glfwPollEvents();
        } else {
            double wait = app.next_wake(now) - now;
            glfwWaitEventsTimeout(wait > 0.001 ? wait : 0.001);
        }
        if (g_wake > 0) g_wake--;
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) { glfwWaitEvents(); continue; }

        anim::active = false;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        ImVec4 bg = ImGui::ColorConvertU32ToFloat4(app.background());
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Capture once animations have had real time to settle.
        static double first_frame = glfwGetTime();
        if (!shot.empty() && ++frames >= 10 && glfwGetTime() - first_frame > 1.0) {
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

    app.shutdown();
    g_app = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
