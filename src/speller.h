#pragma once
// Spellchecking with hunspell. Dictionaries load on a worker thread; words
// are checked on the main thread, remembered by word and by line so that
// typing only re-checks the line being typed on.
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "editor.h"
#include "platform.h"
#include "spell.h"

class Hunspell;

class Speller {
public:
    Speller();
    ~Speller();

    // The dictionaries delta carries: en_US, en_GB.
    static std::vector<std::string> builtin();
    // Every dictionary there is to load: the built-in ones, and each
    // name.aff + name.dic pair in `dir`.
    static std::vector<std::string> available(const fs::path& dir);
    // Starts loading these dictionaries (found as available() finds them) in
    // the background, in place of the ones loaded; until then, nothing is
    // checked. A word is right if any of them knows it, or it's in `words`.
    void load(const std::vector<std::string>& langs, const fs::path& dir, const std::vector<std::string>& words);
    // Drops the dictionaries: nothing is checked.
    void unload();
    // Takes the dictionaries once they've loaded; true from then on.
    bool ready();
    bool loading() const { return worker_.joinable() && !ready_; }
    const std::vector<std::string>& missing() const { return missing_; }  // names not found
    const std::vector<std::string>& errors() const { return errors_; }    // once ready: dictionaries that wouldn't load
    // Changes whenever answers may have: dictionaries loaded, a word added.
    uint64_t generation() const { return gen_; }

    // Known from now on (until the next load).
    void add_word(const std::string& word);
    bool correct(const std::string& word);
    // Likely meant words, best first (each dictionary's, merged).
    const std::vector<std::string>& suggest(const std::string& word);
    // The misspelt words in a prose line.
    const std::vector<WordSpan>& misspelt(const std::string& line);
    // Spelling marks for a note, checking lines not seen before until
    // `budget` seconds have gone (at least one line each time, so it always
    // gets there). Lines left unchecked get no marks yet; false if any were.
    bool check(const std::vector<std::string>& lines, std::vector<Diagnostic>& out, double budget);

private:
    void forget();

    std::vector<std::unique_ptr<Hunspell>> dicts_;
    std::vector<std::string> words_extra_;  // personal words, applied to each dictionary
    std::unordered_map<std::string, bool> words_;
    std::unordered_map<std::string, std::vector<WordSpan>> lines_;
    std::unordered_map<std::string, std::vector<std::string>> suggestions_;
    std::vector<std::string> missing_, errors_;
    bool ready_ = false;
    uint64_t gen_ = 0;

    std::thread worker_;
    std::mutex mu_;
    std::vector<std::unique_ptr<Hunspell>> loaded_;  // the worker's, until taken
    std::vector<std::string> load_errors_;
    bool done_ = false;
};
