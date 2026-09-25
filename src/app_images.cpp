// Images in the app: getting them in (paste, drop, a pasted image URL),
// copying them out, and the full-window viewer. The editor shows them; the
// store keeps them (see images.h).
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <thread>

#include "app.h"
#include "textfile.h"
#include "theme.h"
#include "util.h"

// Downloads give up past these.
static const size_t kMaxDownload = (size_t)25 << 20;
static const int kDownloadTimeoutMs = 15000;

// The bytes of an image file, into the store. Formats delta can show but
// that aren't worth keeping as they are (BMP is huge; TGA, PSD...) become PNG.
// Returns the line that shows it, or "" (with err) if it isn't an image.
std::string App::import_image(const std::string& bytes, std::string& err) {
    std::string ext = image_ext(bytes);
    std::string png;
    if (ext.empty() || ext == "bmp") {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!image_decode_bytes(bytes, rgba, w, h)) { err = "not an image delta can show"; return {}; }
        png = encode_png(rgba, w, h);
        ext = "png";
    }
    std::string link = image_store_.add(png.empty() ? bytes : png, ext, &err);
    return link.empty() ? std::string() : "![](" + link + ")";
}

std::vector<std::string> App::import_files(const std::vector<fs::path>& files, int& skipped) {
    std::vector<std::string> lines;
    skipped = 0;
    for (auto& f : files) {
        std::string bytes, err;
        std::error_code ec;
        if (fs::is_directory(f, ec) || !read_file(f, bytes)) { skipped++; continue; }
        std::string line = import_image(bytes, err);
        if (line.empty()) skipped++;
        else lines.push_back(line);
    }
    return lines;
}

static bool is_web_url(const std::string& s) {
    return (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0) && s.size() > 10 &&
           s.find_first_of(" \t\n") == std::string::npos;
}

// ctrl+v. What's on the clipboard decides, first match wins:
//  1. files copied in a file manager: the images among them;
//  2. image markdown copied from delta: as text (same stored files, no copies);
//  3. image data (a screenshot, "copy image" in a browser): stored as PNG;
//  4. a web address alone, onto an empty line: fetched, and if it turns out
//     to be an image, stored (otherwise it stays the link);
//  5. anything else: text, as ever.
// ctrl+shift+v is always 5.
void App::paste(bool plain) {
    if (!editing()) return;
    if (plain || is_config()) { editor_.paste(); return; }

    std::vector<fs::path> files = clipboard_files();
    if (!files.empty()) {
        int skipped = 0;
        auto lines = import_files(files, skipped);
        if (!lines.empty()) editor_.insert_lines({as_row(lines)});
        if (skipped) status(lines.empty() ? "nothing to paste: those files aren't images" :
                            std::to_string(skipped) + (skipped == 1 ? " file wasn't an image" : " files weren't images"));
        return;
    }

    const char* t = ImGui::GetClipboardText();
    std::string text = t ? t : "";
    // Image markdown (copied in delta): lines of their own, like any image.
    std::vector<std::string> image_lines;
    bool image_md = !text.empty();
    for (size_t a = 0; image_md && a <= text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = trim(text.substr(a, b - a));
        if (!line.empty()) {
            if (md_image_row(line)) image_lines.push_back(line);
            else image_md = false;
        }
        a = b + 1;
    }
    if (image_md && !image_lines.empty() && !editor_.has_selection()) {
        editor_.insert_lines(image_lines);
        return;
    }
    if (!image_md) {
        std::string png, dib, err;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string line;
        if (clipboard_png(png) && image_ext(png) == "png") line = import_image(png, err);
        else if (clipboard_dib(dib) && dib_to_rgba(dib, rgba, w, h)) line = import_image(encode_png(rgba, w, h), err);
        if (!line.empty()) { editor_.insert_lines({line}); return; }
        if (!err.empty()) { status("couldn't paste the image: " + err, true); return; }
    }

    std::string u = trim(text);
    const auto& lines = editor_.lines();
    if (is_web_url(u) && !editor_.has_selection() && trim(lines[editor_.cursor().line]).empty()) {
        fetch_image(u);
        return;
    }
    editor_.paste();
}

// ctrl+c / ctrl+x. With an image (the caret beside it, or selected), the
// clipboard gets the picture as well as its markdown: other apps take the
// picture, another delta note takes the markdown (the same stored file).
// With the caret on an image in a row, it's that one image, not the row.
void App::copy_out(bool cut) {
    int tile = 0;
    int line = editor_.image_for_caret(&tile);
    std::string src = line >= 0 ? editor_.image_src(line, tile) : std::string();
    if (src.empty() || !note_) {
        if (cut) editor_.cut();
        else editor_.copy();
        return;
    }
    std::string md = editor_.image_markdown(line, tile);
    ImGui::SetClipboardText(md.c_str());
    if (cut) editor_.delete_image(line, tile);
    fs::path path = resolve_image(note_->path.parent_path(), src);
    std::string bytes;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (path.empty() || !read_file(path, bytes) || !image_decode_bytes(bytes, rgba, w, h)) return;
    std::string png = image_ext(bytes) == "png" ? bytes : encode_png(rgba, w, h);
    clipboard_put_image(win_, png, rgba_to_dib(rgba, w, h), md);
}

// Several images at once (a paste of files, a drop) land as one row.
std::string App::as_row(const std::vector<std::string>& lines) {
    std::string row;
    for (auto& l : lines) row += (row.empty() ? "" : " ") + l;
    return row;
}

// A pasted web address: a placeholder line right away, the download in the
// background. Only the reply decides whether it's an image.
void App::fetch_image(const std::string& url) {
    auto d = std::make_shared<Download>();
    d->id = next_download_++;
    d->url = url;
    d->marker = std::string("![](") + ImageCache::kPending + std::to_string(d->id) + ")";
    d->note = note_->path;
    editor_.insert_lines({d->marker});
    downloads_.push_back(d);
    std::thread([d] {
        std::string body, type, err;
        bool ok = http_get(d->url, kMaxDownload, kDownloadTimeoutMs, "image/", body, type, err);
        if (ok && image_ext(body).empty()) {
            int w, h;
            if (!image_info_bytes(body, w, h)) { ok = false; err = "that image format can't be shown"; }
        }
        {
            std::lock_guard<std::mutex> lock(d->mu);
            d->ok = ok;
            d->err = err;
            d->bytes = std::move(body);
            d->done = true;
        }
        glfwPostEmptyEvent();
    }).detach();
}

// Downloads that have finished: the placeholder becomes the image, or the
// link again. Quietly, outside undo (undo takes back the paste as a whole).
void App::poll_downloads() {
    for (size_t k = 0; k < downloads_.size();) {
        auto d = downloads_[k];
        std::string bytes, err;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(d->mu);
            if (!d->done) { k++; continue; }
            ok = d->ok;
            err = d->err;
            bytes = std::move(d->bytes);
        }
        downloads_.erase(downloads_.begin() + k);
        std::string line;
        if (ok) line = import_image(bytes, err);
        std::string with = line.empty() ? d->url : line;
        // A web page (not an image) just stays the link it was.
        if (line.empty() && err != "not image/") status("kept the link: " + err, true);
        if (note_ && note_->path == d->note) {
            editor_.replace_line_everywhere(d->marker, with);
        } else if (!line.empty()) {
            // The note was left meanwhile; it was saved with the link.
            rewrite_lines(d->note, [&](auto& l) {
                for (auto& s : l)
                    if (s == d->url) { s = line; return true; }
                return false;
            });
        }
    }
}

// Files dropped on the window: the images among them go where they were
// dropped (the line under the pointer, before or after it by which half).
void App::handle_drops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (auto& drop : drops) {
        if (!is_note()) { status("open a note to drop images into it"); continue; }
        std::vector<fs::path> files;
        for (auto& p : drop.paths) files.push_back(u8path(p));
        int skipped = 0;
        auto lines = import_files(files, skipped);
        if (lines.empty()) { status("nothing to add: those files aren't images"); continue; }
        if (editor_.reader()) editor_.set_reader(false, false);
        else if (drop.at.x >= 0) editor_.jump_to(editor_.position_at(drop.at));  // (negative: at the caret)
        editor_.insert_lines({as_row(lines)});
        if (skipped) status(std::to_string(skipped) + (skipped == 1 ? " file wasn't an image" : " files weren't images"));
    }
}

// ---------------------------------------------------------------------------
// Viewer: the image alone, fitted to the window. It grows out of its spot in
// the note and shrinks back into it.

void App::open_viewer(int line, int tile) {
    std::string src = editor_.image_src(line, tile);
    if (src.empty()) return;
    viewer_ = Viewer{};
    viewer_.open = true;
    viewer_.src = src;
    viewer_.line = line;
    viewer_.tile = tile;
    viewer_.opened_frame = ImGui::GetFrameCount();
    if (!editor_.image_rect(line, tile, viewer_.from_a, viewer_.from_b)) {
        ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
        viewer_.from_a = viewer_.from_b = c;
    }
}

void App::draw_viewer() {
    if (!viewer_.open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 ws = io.DisplaySize;
    int iw = 0, ih = 0;
    if (!images_.size(viewer_.src, iw, ih)) { viewer_.open = false; return; }

    // Keys: esc or space closes, 0 fits again, ctrl+= / ctrl+- zoom. Input
    // moves the targets; what's shown eases after them (below).
    auto zoom_about = [&](ImVec2 at, float factor) {
        float z = std::clamp(viewer_.zoom_to * factor, 0.25f, 32.0f);
        factor = z / viewer_.zoom_to;
        ImVec2 c(ws.x * 0.5f + viewer_.pan_to.x, ws.y * 0.5f + viewer_.pan_to.y);
        // Keep the point under `at` where it is.
        viewer_.pan_to.x += (at.x - c.x) * (1 - factor);
        viewer_.pan_to.y += (at.y - c.y) * (1 - factor);
        viewer_.zoom_to = z;
    };
    bool fresh = ImGui::GetFrameCount() == viewer_.opened_frame;
    if (!viewer_.closing && !fresh) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)) viewer_.closing = true;
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) { viewer_.zoom_to = 1; viewer_.pan_to = ImVec2(0, 0); }
        ImVec2 mid(ws.x * 0.5f, ws.y * 0.5f);
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) zoom_about(mid, 1.25f);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) zoom_about(mid, 0.8f);
        if (io.MouseWheel != 0) zoom_about(io.MousePos, std::pow(1.15f, io.MouseWheel));
        // Left and right: the note's other images, in order.
        int step = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -1 : 0;
        if (step) {
            auto all = editor_.image_list();
            auto it = std::find(all.begin(), all.end(), std::make_pair(viewer_.line, viewer_.tile));
            int at = it == all.end() ? -1 : (int)(it - all.begin());
            int next = at + step;
            if (at >= 0 && next >= 0 && next < (int)all.size()) {
                std::string src = editor_.image_src(all[next].first, all[next].second);
                if (!src.empty() && images_.size(src, iw, ih)) {
                    viewer_.src = src;
                    viewer_.line = all[next].first;
                    viewer_.tile = all[next].second;
                    viewer_.zoom = viewer_.zoom_to = 1;
                    viewer_.pan = viewer_.pan_to = ImVec2(0, 0);
                }
            }
        }
    }
    io.InputQueueCharacters.resize(0);

    // Drag to pan; a click (no drag) off the image closes. Where a click
    // lands is judged against the image where it's headed.
    float margin = std::round(40 * dpi_);
    float fit = std::min({(ws.x - 2 * margin) / iw, (ws.y - 2 * margin) / ih, 1.0f});
    auto rect = [&](float zoom, ImVec2 pan, ImVec2& a, ImVec2& b) {
        float s = fit * zoom;
        ImVec2 c(ws.x * 0.5f + pan.x, ws.y * 0.5f + pan.y);
        a = ImVec2(c.x - iw * s * 0.5f, c.y - ih * s * 0.5f);
        b = ImVec2(c.x + iw * s * 0.5f, c.y + ih * s * 0.5f);
    };
    ImVec2 ta, tb;
    rect(viewer_.zoom_to, viewer_.pan_to, ta, tb);
    if (!viewer_.closing && !fresh) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            viewer_.pressed = true;
            viewer_.dragged = false;
            viewer_.press_at = io.MousePos;
        }
        if (viewer_.pressed && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (std::fabs(io.MousePos.x - viewer_.press_at.x) + std::fabs(io.MousePos.y - viewer_.press_at.y) > 4 * dpi_) viewer_.dragged = true;
            if (viewer_.dragged) {
                viewer_.pan_to.x += io.MouseDelta.x;
                viewer_.pan_to.y += io.MouseDelta.y;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
        }
        if (viewer_.pressed && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            bool on_image = io.MousePos.x >= ta.x && io.MousePos.x < tb.x && io.MousePos.y >= ta.y && io.MousePos.y < tb.y;
            if (!viewer_.dragged && !on_image) viewer_.closing = true;
            viewer_.pressed = false;
        }
    }

    // Grow out of the note, shrink back into it (to wherever it is now).
    if (viewer_.closing) {
        ImVec2 a, b;
        if (editor_.image_rect(viewer_.line, viewer_.tile, a, b) && editor_.image_src(viewer_.line, viewer_.tile) == viewer_.src) {
            viewer_.from_a = a;
            viewer_.from_b = b;
        }
    }
    viewer_.t = anim::approach(viewer_.t, viewer_.closing ? 0.0f : 1.0f, 22.0f, io.DeltaTime, 0.002f);
    if (viewer_.closing && viewer_.t <= 0.0f) { viewer_.open = false; return; }
    float t = viewer_.t;

    // Zoom and pan ease after their targets (expo), by the same fraction
    // each frame: moving together like that, the point under the cursor
    // stays put all the way through a wheel zoom.
    {
        float k = anim::enabled ? 1.0f - std::exp(-30.0f * anim::speed * anim::step(io.DeltaTime)) : 1.0f;
        viewer_.zoom += (viewer_.zoom_to - viewer_.zoom) * k;
        viewer_.pan.x += (viewer_.pan_to.x - viewer_.pan.x) * k;
        viewer_.pan.y += (viewer_.pan_to.y - viewer_.pan.y) * k;
        bool settled = std::fabs(viewer_.zoom_to - viewer_.zoom) < 0.0005f * viewer_.zoom_to &&
                       std::fabs(viewer_.pan_to.x - viewer_.pan.x) < 0.2f && std::fabs(viewer_.pan_to.y - viewer_.pan.y) < 0.2f;
        if (settled) {
            viewer_.zoom = viewer_.zoom_to;
            viewer_.pan = viewer_.pan_to;
        } else {
            anim::active = true;
        }
    }
    float s = fit * viewer_.zoom;
    rect(viewer_.zoom, viewer_.pan, ta, tb);
    auto lerp = [](ImVec2 a, ImVec2 b, float k) { return ImVec2(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k); };
    ImVec2 a = lerp(viewer_.from_a, ta, t), b = lerp(viewer_.from_b, tb, t);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ws, col_alpha(T.bg, t));  // the note steps aside entirely, as for the palette
    float shown = 0;
    ImTextureID tex = images_.texture(viewer_.src, (int)std::ceil(b.x - a.x), shown);
    float round = std::round(3 * dpi_) * (1.0f - t);
    if (tex) dl->AddImageRounded(ImTextureRef(tex), a, b, ImVec2(0, 0), ImVec2(1, 1), col_alpha(IM_COL32_WHITE, std::max(shown, t)), round);
    else dl->AddRectFilled(a, b, col_alpha(T.code_bg, t), round);

    // How far in, when zoomed.
    if (std::fabs(viewer_.zoom - 1) > 0.01f && t > 0.5f) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%d%%", (int)std::lround(s * 100));
        float fs = std::round(base_size() * 0.86f);
        ImVec2 sz = mono_->CalcTextSizeA(fs, FLT_MAX, 0, buf);
        dl->AddText(mono_, fs, ImVec2(ws.x - sz.x - 16 * dpi_, ws.y - sz.y - 12 * dpi_), col_alpha(T.dim, t), buf);
    }
}
