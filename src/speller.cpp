#include "speller.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "filemgr.hxx"
#include "hunspell.hxx"
#include "util.h"

extern const unsigned char g_dict_en_us_aff[];
extern const std::size_t g_dict_en_us_aff_size;
extern const unsigned char g_dict_en_us_dic[];
extern const std::size_t g_dict_en_us_dic_size;
extern const unsigned char g_dict_en_gb_aff[];
extern const std::size_t g_dict_en_gb_aff_size;
extern const unsigned char g_dict_en_gb_dic[];
extern const std::size_t g_dict_en_gb_dic_size;

namespace {

struct Builtin {
    const char* name;
    const unsigned char* aff;
    const std::size_t& aff_size;
    const unsigned char* dic;
    const std::size_t& dic_size;
};

const Builtin builtins[] = {
    {"en_US", g_dict_en_us_aff, g_dict_en_us_aff_size, g_dict_en_us_dic, g_dict_en_us_dic_size},
    {"en_GB", g_dict_en_gb_aff, g_dict_en_gb_aff_size, g_dict_en_gb_dic, g_dict_en_gb_dic_size},
};

const Builtin* find_builtin(const std::string& name) {
    for (auto& b : builtins)
        if (iequals(b.name, name)) return &b;
    return nullptr;
}

// Each name.aff in `dir` with a name.dic beside it: name -> the .aff.
std::vector<std::pair<std::string, fs::path>> user_dicts(const fs::path& dir) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    if (dir.empty()) return out;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2) || lower(path_u8(e.path().extension())) != ".aff") continue;
        fs::path dic = e.path();
        dic.replace_extension(".dic");
        if (fs::exists(dic, ec2)) out.push_back({path_u8(e.path().stem()), e.path()});
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The encoding an .aff file declares (SET), upper-cased; hunspell's default
// is ISO8859-1.
std::string aff_encoding(const std::string& aff) {
    std::istringstream in(aff);
    for (std::string line; std::getline(in, line);) {
        line = trim(line);
        if (line.rfind("SET", 0) == 0 && line.size() > 3 && (line[3] == ' ' || line[3] == '\t')) {
            std::string e = trim(line.substr(4));
            for (char& c : e) c = (char)std::toupper((unsigned char)c);
            return e;
        }
    }
    return "ISO8859-1";
}

std::string latin1_to_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += (char)c;
        } else {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// Rewrites an ISO8859-1 dictionary as UTF-8, so it checks the words delta
// hands it. Single-byte affix flags become one character each, which is
// what FLAG UTF-8 reads.
void to_utf8(std::string& aff, std::string& dic) {
    aff = latin1_to_utf8(aff);
    dic = latin1_to_utf8(dic);
    std::istringstream in(aff);
    std::string out;
    bool has_flag = false;
    for (std::string line; std::getline(in, line);) {
        std::string t = trim(line);
        if (t.rfind("SET", 0) == 0 && t.size() > 3 && (t[3] == ' ' || t[3] == '\t')) continue;
        if (t.rfind("FLAG", 0) == 0 && t.size() > 4 && (t[4] == ' ' || t[4] == '\t')) has_flag = true;
        out += line + "\n";
    }
    aff = std::string("SET UTF-8\n") + (has_flag ? "" : "FLAG UTF-8\n") + out;
}

// Caches are dropped whole when they grow past these: cheap, and a note's
// words come straight back.
const size_t max_words = 200000, max_lines = 50000, max_suggestions = 2000;

}  // namespace

Speller::Speller() = default;

Speller::~Speller() {
    if (worker_.joinable()) worker_.join();
}

std::vector<std::string> Speller::builtin() {
    std::vector<std::string> out;
    for (auto& b : builtins) out.push_back(b.name);
    return out;
}

std::vector<std::string> Speller::available(const fs::path& dir) {
    std::vector<std::string> out = builtin();
    for (auto& [name, aff] : user_dicts(dir))
        if (!find_builtin(name)) out.push_back(name);
    return out;
}

void Speller::forget() {
    words_.clear();
    lines_.clear();
    suggestions_.clear();
}

void Speller::unload() {
    if (worker_.joinable()) worker_.join();
    dicts_.clear();
    forget();
    missing_.clear();
    errors_.clear();
    ready_ = done_ = false;
    gen_++;
}

void Speller::load(const std::vector<std::string>& langs, const fs::path& dir, const std::vector<std::string>& words) {
    unload();
    words_extra_ = words;
    struct Job {
        std::string name;
        const Builtin* builtin = nullptr;
        fs::path aff;
    };
    std::vector<Job> jobs;
    auto users = user_dicts(dir);
    for (auto& l : langs) {
        Job j;
        j.name = l;
        j.builtin = find_builtin(l);
        if (!j.builtin)
            for (auto& [name, aff] : users)
                if (iequals(name, l)) j.aff = aff;
        if (j.builtin || !j.aff.empty()) jobs.push_back(j);
        else missing_.push_back(l);
    }
    worker_ = std::thread([this, jobs] {
        std::vector<std::unique_ptr<Hunspell>> made;
        std::vector<std::string> errors;
        for (auto& j : jobs) {
            std::string aff, dic;
            if (j.builtin) {
                aff.assign((const char*)j.builtin->aff, j.builtin->aff_size);
                dic.assign((const char*)j.builtin->dic, j.builtin->dic_size);
            } else {
                fs::path dic_path = j.aff;
                dic_path.replace_extension(".dic");
                if (!read_file(j.aff, aff) || !read_file(dic_path, dic)) {
                    errors.push_back(j.name + ": couldn't read its files");
                    continue;
                }
                std::string enc = aff_encoding(aff);
                if (enc == "ISO8859-1" || enc == "ISO-8859-1") {
                    to_utf8(aff, dic);
                } else if (enc != "UTF-8") {
                    errors.push_back(j.name + ": written in " + enc + "; delta reads UTF-8 and ISO8859-1 dictionaries");
                    continue;
                }
            }
            // Hunspell reads these names through its file manager, which
            // finds them in memory.
            std::string aff_name = "delta-dict:" + j.name + ".aff", dic_name = "delta-dict:" + j.name + ".dic";
            hunspell_memory_file(aff_name, std::move(aff));
            hunspell_memory_file(dic_name, std::move(dic));
            made.push_back(std::make_unique<Hunspell>(aff_name.c_str(), dic_name.c_str()));
            hunspell_forget_memory_file(aff_name);
            hunspell_forget_memory_file(dic_name);
        }
        std::lock_guard<std::mutex> lock(mu_);
        loaded_ = std::move(made);
        load_errors_ = std::move(errors);
        done_ = true;
    });
}

bool Speller::ready() {
    if (ready_) return true;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!done_) return false;
        dicts_ = std::move(loaded_);
        errors_ = std::move(load_errors_);
    }
    for (auto& d : dicts_)
        for (auto& w : words_extra_) d->add(w);
    forget();
    ready_ = true;
    gen_++;
    return true;
}

void Speller::add_word(const std::string& word) {
    if (word.empty()) return;
    words_extra_.push_back(word);
    for (auto& d : dicts_) d->add(word);
    forget();
    gen_++;
}

bool Speller::correct(const std::string& word) {
    if (!ready_ || dicts_.empty()) return true;  // nothing to check against
    auto it = words_.find(word);
    if (it != words_.end()) return it->second;
    if (words_.size() > max_words) words_.clear();
    bool ok = false;
    for (auto& d : dicts_)
        if ((ok = d->spell(word))) break;
    words_[word] = ok;
    return ok;
}

const std::vector<std::string>& Speller::suggest(const std::string& word) {
    auto it = suggestions_.find(word);
    if (it != suggestions_.end()) return it->second;
    if (suggestions_.size() > max_suggestions) suggestions_.clear();
    std::vector<std::string> out;
    if (ready_)
        for (auto& d : dicts_)
            for (auto& s : d->suggest(word))
                if (out.size() < 8 && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    return suggestions_[word] = std::move(out);
}

const std::vector<WordSpan>& Speller::misspelt(const std::string& line) {
    auto it = lines_.find(line);
    if (it != lines_.end()) return it->second;
    if (lines_.size() > max_lines) lines_.clear();
    std::vector<WordSpan> bad;
    for (auto& w : spell_words(line))
        if (!correct(spell_form(line, w))) bad.push_back(w);
    return lines_[line] = std::move(bad);
}

bool Speller::check(const std::vector<std::string>& lines, std::vector<Diagnostic>& out, double budget) {
    using clock = std::chrono::steady_clock;
    auto until = clock::now() + std::chrono::duration<double>(budget);
    bool checked_one = false, complete = true;
    out.clear();
    std::vector<bool> skip = spell_skipped(lines);
    for (int i = 0; i < (int)lines.size(); i++) {
        if (skip[i]) continue;
        const std::string& s = lines[i];
        if (!lines_.count(s)) {
            if (checked_one && clock::now() >= until) { complete = false; continue; }
            checked_one = true;
        }
        for (auto& w : misspelt(s)) out.push_back({i, w.begin, w.end, "", Diagnostic::Kind::Spelling});
    }
    return complete;
}
