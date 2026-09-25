#include "image_cache.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

#include "images.h"
#include "util.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// Let go of textures not drawn for this long, and of the oldest ones past
// this much memory.
static const double kIdleSeconds = 45.0;
static const size_t kMaxBytes = (size_t)512 << 20;

void ImageCache::start() {
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread([this] { work(); });
}

void ImageCache::shutdown() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
            jobs_.clear();
        }
        cv_.notify_all();
        worker_.join();
    }
    for (auto& [key, e] : entries_) drop(e);
    entries_.clear();
}

void ImageCache::work() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Done d{job.key, {}, 0, 0, false};
        d.ok = image_decode(job.path, job.max_w, d.rgba, d.w, d.h);
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            done_.push_back(std::move(d));
        }
        glfwPostEmptyEvent();  // wake the main loop if it's idle
    }
}

void ImageCache::drop(Entry& e) {
    if (!e.tex) return;
    GLuint t = e.tex;
    glDeleteTextures(1, &t);
    tex_bytes_ -= std::min(tex_bytes_, (size_t)e.tex_w * e.tex_h * 4);
    e.tex = 0;
    e.tex_w = e.tex_h = 0;
    e.ready = -1;
}

// The entry for an image as a note refers to it, looking at the file again
// every couple of seconds so a replaced or newly arrived file shows.
ImageCache::Entry* ImageCache::find(const std::string& src) {
    fs::path p = resolve_image(dir_, src);
    if (p.empty()) return nullptr;
    std::string key = path_u8(p);
    Entry& e = entries_[key];
    if (now_ - e.checked > 2.0) {
        e.checked = now_;
        e.path = p;
        std::error_code ec;
        auto mt = fs::last_write_time(p, ec);
        if (ec) {
            e.ok = false;
            drop(e);
        } else if (!e.ok || mt != e.mtime) {
            e.mtime = mt;
            e.ok = image_info(p, e.w, e.h);
            drop(e);  // changed on disk: decode again
        }
    }
    return &e;
}

std::string ImageCache::label(const std::string& src) {
    return pending(src) ? "downloading" : std::string();
}

bool ImageCache::size(const std::string& src, int& w, int& h) {
    if (pending(src)) {  // a slim bar until the real size is known
        w = 1600;
        h = 160;
        return true;
    }
    Entry* e = find(src);
    if (!e || !e->ok) return false;
    w = e->w;
    h = e->h;
    return true;
}

ImTextureID ImageCache::texture(const std::string& src, int want_w, float& shown) {
    shown = 0.0f;
    if (pending(src)) return ImTextureID{};
    Entry* e = find(src);
    if (!e || !e->ok) return ImTextureID{};
    e->used = now_;
    want_w = std::clamp(want_w, 1, e->w);
    // Decoded a little wider than shown, so small changes (a resize, a zoom
    // step) don't each need a new decode; a bigger view asks for more.
    if (e->tex_w < want_w && e->asked_w < want_w) {
        int ask = std::min(e->w, (int)std::ceil(want_w * 1.25));
        e->asked_w = ask;
        {
            std::lock_guard<std::mutex> lock(mu_);
            jobs_.push_back({path_u8(e->path), e->path, ask});
        }
        cv_.notify_one();
    }
    if (!e->tex) return ImTextureID{};
    float t = std::clamp((float)(now_ - e->ready) / 0.18f, 0.0f, 1.0f);
    shown = anim::enabled ? anim::ease_out(t) : 1.0f;
    return (ImTextureID)(intptr_t)e->tex;
}

void ImageCache::frame(double now) {
    now_ = now;
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lock(mu_);
        done.swap(done_);
    }
    for (auto& d : done) {
        auto it = entries_.find(d.key);
        if (it == entries_.end()) continue;
        Entry& e = it->second;
        e.asked_w = 0;
        if (!d.ok) { e.ok = false; drop(e); continue; }
        bool had = e.tex != 0;
        double ready = e.ready;
        drop(e);
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, d.w, d.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d.rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        e.tex = t;
        e.tex_w = d.w;
        e.tex_h = d.h;
        e.ready = had ? ready : now;  // a sharper copy swaps in without fading again
        tex_bytes_ += (size_t)d.w * d.h * 4;
    }

    // Let go of what hasn't been drawn lately, then the oldest past the cap.
    for (auto& [key, e] : entries_)
        if (e.tex && now - e.used > kIdleSeconds) drop(e);
    while (tex_bytes_ > kMaxBytes) {
        Entry* oldest = nullptr;
        for (auto& [key, e] : entries_)
            if (e.tex && (!oldest || e.used < oldest->used)) oldest = &e;
        if (!oldest) break;
        drop(*oldest);
    }
}
