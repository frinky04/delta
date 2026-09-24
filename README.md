# delta

A fast, minimal markdown note app. C++20, Dear ImGui, GLFW, OpenGL.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/delta.exe
build/delta_tests.exe   # headless editor/markdown tests
```

ImGui 1.92.9b, GLFW 3.4 and stb_image 2.30 are vendored in `third_party/`. Both fonts are embedded in the exe. With MinGW the exe is statically linked, so it runs on its own.

## Spaces

Notes live in `notes_dir` (default `~/Documents/delta`). Each folder directly under it is a **space**, and each `.md` file in a space is a note. Spaces don't nest. Every note belongs to exactly one space, and the palette only shows the current space unless you ask for all of them. Deleted notes go to `notes_dir/.trash`.

Daily notes (`alt+d`) go in the `daily` space, named `YYYY-MM-DD` by default.

## Palette

`ctrl+p` opens it, set in the page's column like a terminal picker: a prompt naming the mode, the match count on the right, and a `>` at the selection (`palette_selection = invert` in `[appearance]` shows a solid bar instead). The first character you type picks the mode and becomes the prompt. Backspace on an empty query goes back to notes.

| prefix | shows |
|---|---|
| *(none)* | notes in the current space; type a new name to create one |
| `*` | notes in every space (`space/name` creates in that space) |
| `#` | spaces; type a new name to create one |
| `@` | headings in this note, starting on the section you're in |
| `:` | go to line (hands over to the status line) |
| `?` | full-text search in this space |
| `/` | every command, with its shortcut |

## Editing

The markdown stays visible and its effect is applied in place: `**bold**` shows its asterisks, dimmed, and the word is bold. `ctrl+e` switches to reader mode, which hides the symbols. In reader mode, clicking a checkbox toggles it and clicking a link follows it. In the editor, ctrl+click a link. `[[note]]` and `[[space/note]]` link to notes, and following one to a missing note creates it. Until then, the link is marked as a problem ("no note with this name yet", with a suggestion if you likely mistyped one), said after its row and, with the caret on it, in the status line. The status bar counts a note's problems when it has any: click the count or press `F8` / `shift+F8` to go to the next or previous one. (Misspelt words are counted apart; see Spelling.) In a note without problems, `F8` takes you to the config's, if it has some.

Notes autosave shortly after you stop typing (`autosave_delay`).

Default keys (all rebindable):

| | |
|---|---|
| `ctrl+p` / `ctrl+alt+p` | find note in this space / any space |
| `ctrl+shift+p`, `f1` | commands |
| `ctrl+shift+s` | switch space |
| `ctrl+shift+o` / `ctrl+g` | jump to heading / line |
| `ctrl+f` / `ctrl+h` | find / find and replace in this note |
| `ctrl+shift+f` | search every note in this space |
| `ctrl+↑` / `ctrl+↓` | previous / next paragraph (add shift to select) |
| `ctrl+alt+↑` / `ctrl+alt+↓` | previous / next heading |
| `alt+shift+→` / `alt+shift+←` | expand / shrink selection |
| `alt+enter` | follow the link at the caret |
| `f8` / `shift+f8` | next / previous problem |
| `alt+f8` / `alt+shift+f8` | next / previous misspelt word |
| `alt+.` | fix the nearest misspelt word with its best guess (again: the next guess) |
| `ctrl+.` | fix spelling, word by word |
| `ctrl+n` / `alt+d` / `f2` | new note / today's daily note / rename |
| `alt+left` / `alt+right` | back / forward through opened notes |
| `ctrl+e` | reader mode |
| `alt+up` / `alt+down` | move line(s); with an image picked, the image |
| `shift+alt+up` / `shift+alt+down` | duplicate line(s) |
| `ctrl+shift+k` / `ctrl+l` | delete line / select line |
| `ctrl+enter` | toggle task checkbox |
| `ctrl+b` `ctrl+i` `` ctrl+` `` `ctrl+shift+x` | bold, italic, code, strikethrough (toggles) |
| `tab` / `shift+tab`, `ctrl+]` / `ctrl+[` | indent / outdent |
| `ctrl+=` `ctrl+-` `ctrl+0` | zoom |
| `f11` | full screen |
| `ctrl+,` | edit the config |

Typing `*`, `_`, `` ` ``, `~`, `=`, `(`, `[` or `"` with text selected wraps the selection. `ctrl+c` or `ctrl+x` with nothing selected copies or cuts the whole line. Pasting a URL over selected text makes it a link. Numbered lists renumber themselves as items are added, removed or moved. Each note reopens with the caret and scroll where you left them.

Tools that act on the note in view live in the status line, so the note stays in sight; `esc` leaves any of them.


- **Find** (`ctrl+f`, `ctrl+h`): matches are lit as you type (case-sensitive only if you type a capital), `enter` / `shift+enter` step through them, `tab` switches between find and replace, `enter` in replace replaces the current match, `ctrl+alt+enter` replaces them all (one undo). `esc` closes with the match selected.
- **Go to line** (`ctrl+g`): `42`, `42:10` for a column, or `+5` / `-5` from where you are. The page follows as you type; `enter` stays there, `esc` goes back.
- **Rename** (`f2`): the note's name in the status line becomes editable where it sits, checked as you type.
- **Fix spelling** (`ctrl+.`): the misspelt word at the caret (or the next one) is selected and its best guess fills the field; `tab` / `shift+tab` goes through the other suggestions, or type your own. `enter` fixes it and moves to the next, `shift+enter` skips it, `ctrl+enter` adds it to your dictionary. The pass ends back where it started.

Choosing from a list stays in the palette, headings included (`ctrl+shift+o`, or `[sections]` in the config).

Expand selection grows word, sentence, line, paragraph, section (under its heading), then the whole note.

## Spelling

Misspelt words get a dotted underline as you write (`spelling` in `[colors]`). The word you're typing isn't marked until you've moved on from it. Only prose is checked: code, links' targets, URLs, paths, emails, `#tags`, `snake_case` and `camelCase` names, ALL CAPS, words with digits and front matter are left alone. The status bar counts the misspelt words apart from problems: click the count or press `alt+F8` / `alt+shift+F8` to go through them. With the caret on one, its likeliest fix shows in the middle of the status line ("Disempower?"). `alt+.` puts it in without opening anything: it fixes the misspelt word at the caret, or the nearest one before it in the paragraph, and leaves the caret where it was, so you can fix a typo a few words back and keep typing. Press it again for the next guess (the last one is the word as you wrote it); one `ctrl+z` undoes it whichever guess it is on. `ctrl+.` opens the fix tool in the status line, then the rest of the note one word at a time (see the status-line tools under Editing).

English (`en_US`, and `en_GB` with -ise spellings) is built in. `[spell]` in the config picks the dictionaries, and a space can have its own:

```ini
[spell]
languages = en_GB            ; several: en_GB, de_DE (a word is right if any knows it)
space.recipes = de_DE        ; this space checks German instead
enabled = true
```

For other languages, put a hunspell dictionary (`de_DE.aff` and `de_DE.dic`, from LibreOffice's or Firefox's dictionaries, say) in the `dictionaries` folder next to the config and use its name. UTF-8 and ISO8859-1 dictionaries work. Your own words go in `words.txt` next to the config, one a line; edits to it apply within a second. "Toggle spellcheck" in the `/` palette turns it off until the config says otherwise.

The dictionaries are SCOWL's (http://wordlist.aspell.net), see `assets/dict/README_SCOWL.txt`; spellchecking is Hunspell's, see `third_party/hunspell`.

## Images

A line holding just an image, `![](path)`, shows the image itself: at its own pixel size (a screenshot shows 1:1), no wider than the column and no taller than most of the window. The caret never rests on it: arrowing or clicking onto an image picks it whole, ringed in the accent colour. Arrows step on from it (up and down between rows, too), typing or `enter` starts a new line after it, `space` views it, and backspace or delete removes it (undoable). A selection running over it tints it. Reader mode shows it the same way. An image that can't be shown stays as text, with a quiet note saying why. Showing a note never touches the network: web images are only ever fetched once, when you paste one.

Getting images in:

- **Paste** (`ctrl+v`). Image files copied in Explorer, a screenshot, or "copy image" in a browser all become images in the note. Pasting a web address on its own onto an empty line fetches it in the background (a placeholder holds its spot); if it turns out to be an image it's stored, otherwise it stays the link. `ctrl+shift+v` pastes plain text: nothing is fetched.
- **Drop** image files onto the note, and they go where you drop them.

Pasted and dropped images land on lines of their own (a line with text is split around them), and one undo takes them back. Several at once land as a row. BMPs and other bulky formats are stored as PNG.

**Sizing**, as Obsidian writes it: `![|300](path)` is 300px wide (it scales with zoom), `![|300x200](path)` fits a box. You don't type it: with an image picked (or the pointer over it) a handle sits on its right edge; drag it, and it snaps to a quarter, half, three quarters or all of the column, or the image's own size. Double-click the handle for its own size again.

**Rows**: images on the same line, `![](a.png) ![](b.png)`, sit side by side, all one height, the row exactly as wide as the column; past four they wrap into more rows. Other markdown tools show same-line images side by side too. Arrows pick image by image; backspace and delete take out the picked one; typing with one picked splits the row after it; `ctrl+alt+←` / `ctrl+alt+→` swap the picked image with its neighbour, and `alt+↑` / `alt+↓` move it a step up or down: into the row above or below when a line wraps into rows, out of its row onto a line of its own, or from a line of its own into the next row of images (at its end going up, its start going down, keeping reading order). Next to text, its line moves as any line does; to move a whole row, select its line (`ctrl+l`) first. Drag an image by its body onto another's left or right half to join its row there, or between lines to give it a line of its own; that's also how you reorder them.

**Copying** an image (picked, or selected) puts the picture on the clipboard as well as its markdown: other apps get the picture, another note gets the same stored file.

**Viewing**: double-click an image, press `space` beside it, or click it in reader mode. It grows to fill the window; the wheel or `ctrl+=` / `ctrl+-` zoom, drag pans, `0` fits it again, `←` / `→` go through the note's other images, and `esc`, `space` or a click outside puts it back.

Images live out of sight in one store for the whole notes folder, `.delta/images` (hidden), named by a hash of their contents, so the same image is stored once and renaming or moving notes never touches it. Notes refer to them with ordinary markdown, `![](../.delta/images/<hash>.png)`, which other markdown tools show too. PNG, JPEG, GIF (first frame), BMP, TGA and PSD are read. Large images are decoded in the background, scaled down to the size they're shown at.

## Config

Everything lives in one ini file: `%APPDATA%\delta\delta.ini` on Windows, `~/.config/delta/delta.ini` elsewhere. It starts out as just the section headers: it holds only what you've changed, and anything left out uses its default.

`ctrl+,` opens it in delta itself, as ini text in mono. It autosaves like a note, and every save applies straight away. Beside each setting, delta shows what it does and its default (or that it already is the default), so the file itself never needs to carry them. "Add a setting" in the `/` palette lists everything the file doesn't set yet, settings and key bindings alike; pick one and it's written into its section at its default, with the value selected. Problems are shown at the end of their line instead, with a suggestion where there is one: a misspelt name ("did you mean line_height?"), a setting in the wrong section, a value out of range, an unreadable key chord. Each says what delta uses instead, and the part it's about is underlined. They're checked as you type; a problem on the line you're typing on waits for a pause. The status bar counts them: click the count or press `F8` / `shift+F8` to go to the next or previous one, with the bad part selected. Problem marks use `error` in `[colors]`. `ctrl+shift+o` jumps between `[sections]`, and `alt+left` takes you back to your note. To use another editor, run "Open config in another app" from the `/` palette. Changes made there are picked up within a second.

Any palette command can be bound in `[keys]` by its id ("Add a setting" lists them):

```ini
[keys]
note.daily = alt+d ctrl+shift+d   ; several chords, space-separated
edit.bold =                       ; empty unbinds
```

Window position, the last open note and zoom live in `state.ini` next to it, so the app never rewrites your config.

## Command line

`--config <path>` uses another config. `--run a,b` runs command ids at start-up (also `open:<note>` and `palette:<text>`). `--screenshot out.png --size WxH` renders and exits.
