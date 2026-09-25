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

- `.` notes (`ctrl+o`)
- `/` spaces
- `@` headings in this note
- `:` line number
- `?` text in notes
- `[` notes that link to this one
- `#` notes by tag (`ctrl+t`)

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

## Images

Paste or drop an image into a note to add it. Drag its right edge to resize it. Images on the same line form a row. Double-click an image to view it full size.

Images are saved in the notes folder, once each, and linked with standard markdown.

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
