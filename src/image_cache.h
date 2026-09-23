#pragma once
// The images the editor shows, as textures. Sizes come from file headers
// (fast, so layout never waits); pixels are decoded on a worker thread,
// scaled to the size they're shown at, and uploaded on the main thread.
// Textures not drawn for a while are let go.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "editor.h"
#include "platform.h"

class ImageCache : public ImageSource {
public:
    ~ImageCache() override { shutdown(); }
    void start();
    void shutdown();                    // GL context must still be current
    void set_note_dir(const fs::path& dir) { dir_ = dir; }

    bool size(const std::string& src, int& w, int& h) override;
    ImTextureID texture(const std::string& src, int want_w, float& shown) override;
    std::string label(const std::string& src) override;

    // Downloads in progress are written as ![](delta-pending:<n>) until they
    // land; they show as a placeholder saying so.
    static constexpr const char* kPending = "delta-pending:";
    static bool pending(const std::string& src) { return src.rfind(kPending, 0) == 0; }

    // Once per frame on the main thread: uploads what the worker decoded and
    // frees textures that haven't been drawn for a while.
    void frame(double now);

private:
    struct Entry {
        fs::path path;
        bool ok = false;                // a readable image
        int w = 0, h = 0;               // full size
        fs::file_time_type mtime{};
        double checked = -1e9;          // when the file was last looked at
        unsigned tex = 0;               // GL texture, 0 = none
        int tex_w = 0, tex_h = 0;
        int asked_w = 0;                // width being decoded, 0 = none
        double ready = -1;              // when it first appeared (fade-in)
        double used = 0;                // last drawn
    };
    struct Job { std::string key; fs::path path; int max_w; };
    struct Done { std::string key; std::vector<uint8_t> rgba; int w, h; bool ok; };

    Entry* find(const std::string& src);
    void work();
    void drop(Entry& e);

    fs::path dir_;
    std::unordered_map<std::string, Entry> entries_;
    size_t tex_bytes_ = 0;
    double now_ = 0;

    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Done> done_;
    bool stop_ = false;
};
