#pragma once
// Spellchecking with hunspell. Dictionaries load on a worker thread; words
// are checked on the main thread, remembered by word and by line so that
// typing only re-checks the line being typed on. Suggestions (tens of
// milliseconds each) can be asked for without waiting: another worker finds
// them. One lock guards the dictionaries, and the main thread's checking
// never waits on it (it tries again next frame).
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
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
    bool loading() const { return loader_.joinable() && !ready_; }
    const std::vector<std::string>& missing() const { return missing_; }  // names not found
    const std::vector<std::string>& errors() const { return errors_; }    // once ready: dictionaries that wouldn't load
    // Changes whenever answers may have: dictionaries loaded, a word added.
    uint64_t generation() const { return gen_; }

    // Known from now on (until the next load).
    void add_word(const std::string& word);
    bool correct(const std::string& word);
    // Likely meant words, best first (each dictionary's, merged). Waits for
    // them: for when you've asked.
    std::vector<std::string> suggest(const std::string& word);
    // The same without waiting: the list if it's known, or nothing while the
    // worker finds it (the newest word asked for wins).
    std::optional<std::vector<std::string>> suggest_later(const std::string& word);
    // Whether the worker is still finding suggestions (so it's worth another frame).
    bool suggesting() const { return busy_; }
    // Spelling marks for a note, checking lines not seen before until
    // `budget` seconds have gone (at least one line each time, so it always
    // gets there, unless the suggestion worker has the dictionaries: then
    // new lines wait). Lines left unchecked get no marks yet; false if any were.
    bool check(const std::vector<std::string>& lines, std::vector<Diagnostic>& out, double budget);

private:
    void forget();
    // These use the dictionaries: dict_mu_ must be held.
    bool correct_locked(const std::string& word);
    const std::vector<WordSpan>& misspelt(const std::string& line);
    std::vector<std::string> find_suggestions(const std::string& word);
    void suggest_worker();

    std::mutex dict_mu_;  // dicts_ and what hunspell does with them
    std::vector<std::unique_ptr<Hunspell>> dicts_;
    std::vector<std::string> words_extra_;  // personal words, applied to each dictionary
    std::unordered_map<std::string, bool> words_;
    std::unordered_map<std::string, std::vector<WordSpan>> lines_;
    std::vector<std::string> missing_, errors_;
    bool ready_ = false;
    uint64_t gen_ = 0;

    // Loading.
    std::thread loader_;
    std::mutex load_mu_;
    std::vector<std::unique_ptr<Hunspell>> loaded_;  // the loader's, until taken
    std::vector<std::string> load_errors_;
    bool done_ = false;

    // Suggesting: the cache, and the word the worker's asked for. A result
    // is kept only if nothing was forgotten meanwhile (same sug_gen_).
    std::mutex sug_mu_;
    std::condition_variable sug_cv_;
    std::unordered_map<std::string, std::vector<std::string>> suggestions_;
    std::string sug_want_, sug_doing_;  // asked for next, being found now
    bool sug_has_want_ = false, sug_stop_ = false;
    uint64_t sug_gen_ = 0;
    std::atomic<bool> busy_{false};
    std::thread suggester_;
};
