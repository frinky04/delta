#include "import.h"

#include <algorithm>

#include "markdown.h"
#include "util.h"

static std::string ext_of(const fs::path& p) { return lower(path_u8(p.extension())); }

bool is_doc_file(const fs::path& p) {
    std::string e = ext_of(p);
    return e == ".md" || e == ".txt";
}

bool is_image_file(const fs::path& p) {
    // What the image decoder reads (see images.cpp).
    static const char* exts[] = {".png", ".jpg", ".jpeg", ".jpe", ".gif", ".bmp", ".tga", ".psd", ".hdr", ".pic", ".pnm", ".ppm", ".pgm"};
    std::string e = ext_of(p);
    return std::any_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; });
}

DropPlan plan_drop(const std::vector<fs::path>& paths, const std::function<bool(const fs::path&)>& is_dir) {
    DropPlan out;
    for (auto& p : paths) {
        if (is_dir(p)) out.folders.push_back(p);
        else if (is_doc_file(p)) out.docs.push_back(p);
        else if (is_image_file(p)) out.images.push_back(p);
        else out.others.push_back(p);
    }
    return out;
}

FolderPlan plan_folder(const std::vector<FolderEntry>& entries) {
    FolderPlan out;
    for (auto& e : entries) {
        if (e.name.empty() || e.name[0] == '.') continue;
        if (e.dir) continue;
        if (is_doc_file(u8path(e.name))) out.docs.push_back(e.name);
        else out.left.push_back(e.name);
    }
    std::sort(out.docs.begin(), out.docs.end(), [](const std::string& a, const std::string& b) { return natural_less(a, b); });
    std::sort(out.left.begin(), out.left.end(), [](const std::string& a, const std::string& b) { return natural_less(a, b); });
    return out;
}

std::string doc_note_name(const fs::path& file) {
    std::string name = trim(path_u8(file.stem()));
    // Characters a note name can't have (a file from elsewhere may).
    for (char& c : name)
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || (unsigned char)c < 32) c = '-';
    size_t a = name.find_first_not_of(". "), b = name.find_last_not_of(". ");
    name = a == std::string::npos ? std::string() : name.substr(a, b - a + 1);
    if (name.size() > 190) {  // room for a suffix
        size_t n = 190;
        while (n > 0 && utf8::is_cont((unsigned char)name[n])) n--;
        name = trim(name.substr(0, n));
    }
    return name.empty() ? "untitled" : name;
}

std::string free_name(const std::string& want, const std::function<bool(const std::string&)>& taken) {
    if (!taken(want)) return want;
    for (int i = 2;; i++) {
        std::string n = want + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

bool is_local_src(const std::string& s) {
    if (s.empty() || s.find("://") != std::string::npos) return false;
    std::string l = lower(s);
    for (const char* scheme : {"data:", "mailto:", "delta-pending:", "file:"})
        if (l.rfind(scheme, 0) == 0) return false;
    if (s[0] == '/' || s[0] == '\\' || s[0] == '#' || s[0] == '?') return false;
    if (s.size() >= 2 && std::isalpha((unsigned char)s[0]) && s[1] == ':') return false;  // C:\...
    return true;
}

// Lines of front matter at the top (--- ... ---), fences included: 0 if none.
static int front_matter_end(const std::vector<std::string>& lines) {
    if (lines.empty() || trim(lines[0]) != "---") return 0;
    for (int i = 1; i < (int)lines.size(); i++) {
        std::string t = trim(lines[i]);
        if (t == "---" || t == "...") return i + 1;
    }
    return 0;
}

std::vector<DocImage> local_images(const std::vector<std::string>& lines) {
    std::vector<DocImage> out;
    std::vector<bool> code = md_code_lines(lines);
    for (int i = front_matter_end(lines); i < (int)lines.size(); i++) {
        if (i < (int)code.size() && code[i]) continue;
        const std::string& s = lines[i];
        for (size_t k = 0; k < s.size();) {
            if (s[k] == '\\') { k += 2; continue; }
            // Inline code: past the run of backticks that closes it.
            if (s[k] == '`') {
                size_t run = s.find_first_not_of('`', k);
                if (run == std::string::npos) break;
                std::string ticks = s.substr(k, run - k);
                size_t close = s.find(ticks, run);
                k = close == std::string::npos ? run : close + ticks.size();
                continue;
            }
            ImageRef ref;
            if (s[k] == '!' && md_image_at(s, k, ref)) {
                if (is_local_src(ref.src)) out.push_back({i, ref.src_at, (int)ref.src.size(), ref.src});
                k = ref.end;
                continue;
            }
            k++;
        }
    }
    return out;
}

void rewrite_images(std::vector<std::string>& lines, const std::vector<DocImage>& images,
                    const std::map<std::string, std::string>& to) {
    // Last first, so where the earlier ones are stays true.
    std::vector<DocImage> order = images;
    std::sort(order.begin(), order.end(), [](const DocImage& a, const DocImage& b) {
        return a.line != b.line ? a.line > b.line : a.at > b.at;
    });
    for (auto& im : order) {
        auto it = to.find(im.src);
        if (it == to.end() || im.line < 0 || im.line >= (int)lines.size()) continue;
        std::string& l = lines[im.line];
        if (im.at < 0 || im.at + im.len > (int)l.size() || l.compare(im.at, im.len, im.src) != 0) continue;
        l.replace(im.at, im.len, it->second);
    }
}
