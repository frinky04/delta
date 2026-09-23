# delta

A fast, minimal markdown note app. C++20, Dear ImGui, GLFW, OpenGL.

## Build

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/delta.exe
build/delta_tests.exe   # headless editor/markdown tests
```

ImGui 1.92.9b and GLFW 3.4 are vendored in `third_party/`. Both fonts are embedded in the exe. With MinGW the exe is statically linked, so it runs on its own.

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
| `@` | headings in this note |
| `:` | go to line |
| `?` | full-text search in this space |
| `/` | every command, with its shortcut |

## Editing

The markdown stays visible and its effect is applied in place: `**bold**` shows its asterisks, dimmed, and the word is bold. `ctrl+e` switches to reader mode, which hides the symbols. In reader mode, clicking a checkbox toggles it and clicking a link follows it. In the editor, ctrl+click a link. `[[note]]` and `[[space/note]]` link to notes, and following one to a missing note creates it.

Notes autosave shortly after you stop typing (`autosave_delay`).

Default keys (all rebindable):

| | |
|---|---|
| `ctrl+p` / `ctrl+alt+p` | find note in this space / any space |
| `ctrl+shift+p`, `f1` | commands |
| `ctrl+shift+s` | switch space |
| `ctrl+shift+o` / `ctrl+g` | jump to heading / line |
| `ctrl+f` | search this space |
| `ctrl+n` / `alt+d` / `f2` | new note / today's daily note / rename |
| `alt+left` / `alt+right` | back / forward through opened notes |
| `ctrl+e` | reader mode |
| `alt+up` / `alt+down` | move line(s) |
| `shift+alt+up` / `shift+alt+down` | duplicate line(s) |
| `ctrl+shift+k` / `ctrl+l` | delete line / select line |
| `ctrl+enter` | toggle task checkbox |
| `ctrl+b` `ctrl+i` `` ctrl+` `` `ctrl+shift+x` | bold, italic, code, strikethrough (toggles) |
| `tab` / `shift+tab`, `ctrl+]` / `ctrl+[` | indent / outdent |
| `ctrl+=` `ctrl+-` `ctrl+0` | zoom |
| `f11` | full screen |
| `ctrl+,` | edit the config |

Typing `*`, `_`, `` ` ``, `~`, `=`, `(`, `[` or `"` with text selected wraps the selection. `ctrl+c` or `ctrl+x` with nothing selected copies or cuts the whole line.

## Config

Everything lives in one ini file: `%APPDATA%\delta\delta.ini` on Windows, `~/.config/delta/delta.ini` elsewhere. It's created on first run with every setting listed, commented out at its default. Remove the `;` to change one.

`ctrl+,` opens it in delta itself, as ini text in mono. It autosaves like a note, and every save applies straight away. Problems are shown at the end of their line (an unknown setting, a value that isn't a number, an unreadable key chord), and the status bar counts them. `@` in the palette jumps between `[sections]`, and `alt+left` takes you back to your note. To use another editor, run "Open config in another app" from the `/` palette. Changes made there are picked up within a second.

The `[keys]` section lists every command id, so any palette command can be bound:

```ini
[keys]
note.daily = alt+d ctrl+shift+d   ; several chords, space-separated
edit.bold =                       ; empty unbinds
```

Window position, the last open note and zoom live in `state.ini` next to it, so the app never rewrites your config.

## Command line

`--config <path>` uses another config. `--run a,b` runs command ids at start-up (also `open:<note>` and `palette:<text>`). `--screenshot out.png --size WxH` renders and exits.
