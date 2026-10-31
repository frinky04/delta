# delta

A keyboard-driven markdown note app, modelled on a code editor. Notes are plain `.md` files.

Press `ctrl+p` to list every command and its key. Press `f1` to open this manual.

## Notes and spaces

Notes are kept in `~/Documents/delta`. Each folder there is a **space**. Each `.md` file in a space is a note.

- `ctrl+n` makes a new note.
- `alt+d` opens today's daily note.
- `ctrl+tab` goes to the note you had open before.

Notes save as you type. A deleted note goes to the trash; use "restore deleted note" to get it back.

If a note changes on disk while you have unsaved edits, delta stops saving it. Press `ctrl+s` to keep your version, or use "reload note" to take the file's. If you leave without choosing, your version is saved beside the file as "name (yours)".

## Palette

`ctrl+p` opens the palette with commands. Type a leading character to find something else; backspace returns to commands.

| Type | To find | Key |
| :--: | --- | --- |
| `.` | notes | `ctrl+o` |
| `/` | spaces | |
| `@` | headings in this note | |
| `:` | a line number | |
| `?` | text in notes | |
| `[` | notes that link to this one | |
| `#` | notes by tag | `ctrl+t` |

Notes, text and tags search the current space. Press `tab` to search every space. Type `work/plan` to look in one space; if the note doesn't exist, delta makes it. Press `ctrl+r` to sort notes by name or by most recent. A mode shows its own keys beside the prompt until you type.

## Writing

Markdown symbols stay visible, dimmed, with the formatting applied. Press `ctrl+e` for reader mode, which hides them.

Find (`ctrl+f`), go to line (`ctrl+g`) and rename (`f2`) open in the status line at the bottom.

## Links

Write `[[note]]` or `[[space/note]]` to link to a note. Press `alt+enter`, or `ctrl`+click, to follow a link; in reader mode a plain click works too. If the note doesn't exist, delta makes it. Press `alt+left` and `alt+right` to go back and forward.

While you type a `[[link]]` or a `#tag`, matches show under the caret. Press `enter` to take one, or `esc` to hide them.

## Tags

A note is in one space, but it can have any number of tags: `#idea`, `#reading-list`, `#project/delta`. A tag must contain a letter, so `#12` is not a tag. Case doesn't matter. Tags inside code are ignored.

- `ctrl+t` lists tags, with how many notes have each.
- `#rust #idea` finds notes with both tags.
- `-#done` leaves out notes with that tag.
- `alt+enter` or `ctrl`+click on a tag lists its notes.

To rename a tag in every note of every space, use "rename tag", or put the caret on the tag and press `f2`. delta asks before it changes anything. Renaming to a tag that exists merges the two.

## Spelling

Misspelt words are underlined, and the likeliest fix shows in the status line.

- `alt+.` applies that fix.
- `ctrl+.` goes through the note one word at a time. In it, `shift+enter` skips a word and `ctrl+enter` adds it to your dictionary.

Code, links and names are not checked. US and UK English are built in.

## Style

delta marks small slips the way a code formatter does, with faint dashes under them: double spaces, spaces at the ends of lines, extra blank lines, a space before a comma, no space after a full stop, a word typed twice, a bracket or quote left open, no blank line around a code block, and a list that mixes `-`, `*` and `+` or indents unevenly. Code, links, urls, tags and front matter are left alone. Two or more spaces at the end of a line stay: in markdown they are a line break.

- `alt+.` fixes the slip at the caret. The status line says what it is.
- `f8` steps through slips along with the note's other problems.
- "format note" (`shift+alt+f`) fixes every slip in the note at once, and one undo puts them all back. A bracket or quote left open is for you to fix.

To hide the marks, set `lint = false`. To skip some rules, list them in `lint_off`; by default it skips `final_newline` (a note that doesn't end in a new line) and `heading_blank` (no blank line around a heading). Listing your own replaces those, so keep them in the list if you still want them skipped. To format each time you press `ctrl+s`, set `format_on_save = true`; saving on its own as you type never changes your text.

```ini
[editor]
lint_off = final_newline, heading_blank, repeated_word
format_on_save = true
```

The rules are `double_space`, `trailing_space`, `blank_lines`, `space_before_punct`, `space_after_punct`, `final_newline`, `repeated_word`, `unclosed`, `heading_blank`, `fence_blank`, `list_marker` and `list_indent`.

## Tables

Write a table in markdown, one row per line, with `|` between cells and a row of dashes under the header. Colons in that row align a column: `:--` left, `:-:` center, `--:` right.

```
| Key | Does |
| --- | ---- |
| `ctrl+e` | reader mode |
```

which reads as:

| Key | Does |
| --- | ---- |
| `ctrl+e` | reader mode |

In reader mode a table shows as a grid. Columns take the room their cells need, and when the note is too narrow, long cells wrap within their column. Links in cells work as they do anywhere else.

## Images

Paste or drop an image into a note to add it. Drag its right edge to resize it. Images on the same line form a row. Double-click an image to view it full size.

Images are saved in the notes folder, once each, and linked with standard markdown.

## Docs from elsewhere

Drop a `.md` or `.txt` file on the window to read it without adding it to your notes. It opens in reader mode, and the status line shows `visiting / report.md`. A visiting doc is read only: it is never saved, and it stays out of search, tags, links and recent notes. Images beside it show as they would in its own folder. Open another note and it's gone. Opening a file with delta (`delta report.md`, or "open with" in your file manager) works the same way.

To keep a visiting doc, use "keep note" (`ctrl+s` while visiting) and pick a space. delta copies the file in as a note and opens it. The original is never moved or changed.

Drop several files, or a folder, to import them in one go: pick a space, or type a new space's name. A dropped folder can also become a space of its own, named after the folder. Only the `.md` and `.txt` files at the folder's top level come in; sub-folders are passed over, since spaces don't nest.

When a doc comes in:

- A `.txt` file becomes a `.md` note with its text as it was. Front matter stays as it is.
- If the name is taken, the note gets the next free one, like "report 2". The status line says so.
- Images it shows from files beside it, like `![](img/chart.png)`, are copied into your notes and relinked. Web images and absolute paths are left as they are. An image that can't be found stays as written, and the status line counts it.

Images dropped along with docs go into the open note, as ever. Other files are skipped, and the status line says how many.

## Config

Press `ctrl+,` to open the config. It holds only the settings you change. Each line shows its default, and mistakes are marked as you type. Use "add setting" to list every setting and key.

```ini
[editor]
font = Georgia

[spell]
languages = en_GB, de_DE

[keys]
note.daily = ctrl+shift+d
```

A key works where its command does: `ctrl+r` in the notes list, `f2` on a tag. If you give one key to two commands, the config marks it and says which one runs.

For other languages, put a hunspell dictionary (for example `de_DE.aff` and `de_DE.dic`) in the `dictionaries` folder next to the config.

## Themes

Use "choose theme" to preview each theme as you move over it. delta has its own dark and light themes, and ports of monkeytype's serika dark, serika, carbon, 8008, olivia, bento and moonlight, and of nord, dracula, gruvbox, catppuccin, rosé pine, tokyo night and everforest.

Colors under `[colors]` apply on top of the theme:

```ini
[appearance]
theme = nord

[colors]
accent = #ebcb8b
```

## Files

- Notes: `~/Documents/delta`, or `notes_dir` under `[general]` in the config.
- Config: `delta.ini` in `%APPDATA%\delta` on Windows, or `~/.config/delta` elsewhere.
- Your dictionary: `words.txt`, next to the config.

## Building

Needs CMake, Ninja and a C++20 compiler. All libraries are in `third_party`.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The app is `build/delta`.

## Credits

Dictionaries from SCOWL (http://wordlist.aspell.net). Spellchecking by Hunspell.

Portions of this software are copyright © 2024 The FreeType Project (https://freetype.org). All rights reserved.
