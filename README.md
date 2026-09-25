# delta

A keyboard-driven markdown note app, modelled on a code editor. Notes are plain `.md` files.

`ctrl+p` lists every command with its key, so start there. `f1` shows this page.

## Notes and spaces

Notes live in `~/Documents/delta`. Each folder there is a **space**, and each `.md` file in it is a note.

- `ctrl+n` new note
- `alt+d` today's daily note
- `ctrl+tab` the note before

Notes save as you type. Deleted notes go to the trash, and "restore deleted note" brings one back.

If a note changes on disk while it has unsaved edits, delta stops saving it until you choose: `ctrl+s` keeps yours, "reload note" takes the file's. Leave without choosing and yours are kept beside it as "name (yours)".

## Palette

A leading character switches what the palette finds, and backspace goes back to commands:

- `.` notes (`ctrl+o`)
- `/` spaces
- `@` headings in this note
- `:` line number
- `?` text
- `[` notes that link here
- `#` notes by tag (`ctrl+t`)

Notes, text and tags start in this space; `tab` widens them to every space. `work/plan` looks in one space, and a name that doesn't exist makes the note. `ctrl+r` sorts notes by name or most recent. A mode's own keys show beside the prompt until you type.

## Writing

Markdown symbols stay visible, dimmed, with the formatting applied. `ctrl+e` toggles reader mode, which hides them.

`[[note]]` and `[[space/note]]` link to notes, and following a link to a missing note makes it. `alt+left` and `alt+right` go back and forward. While you type a `[[link]]` or a `#tag`, matches come up under the caret: `enter` takes one, `esc` puts them away.

Find (`ctrl+f`), go to line (`ctrl+g`) and rename (`f2`) open in the status line.

## Tags

A note lives in one space but can have any number of tags: `#idea`, `#reading-list`, `#project/delta`. A tag needs a letter, so `#12` stays a number. Case doesn't matter, and code isn't searched.

`ctrl+t` lists tags and how many notes have each. `#rust #idea` finds notes with both, and `-#done` leaves out notes with that tag. Following a tag (`alt+enter` or `ctrl`+click) lists its notes.

"rename tag" renames a tag in every note, in every space, after asking. With the caret on a tag, `f2` renames that tag instead of the note. Renaming to a tag that exists merges them.

## Spelling

Misspelt words are underlined, and the likeliest fix shows in the status line. `alt+.` applies it; `ctrl+.` goes through the note word by word. Code, links and names aren't checked. US and UK English are built in.

## Images

Paste or drop an image to add it; drag its right edge to resize it. Images on one line form a row. Double-click one to view it full size. They're stored in the notes folder, once each, as standard markdown.

## Config

`ctrl+,` opens the config, which holds only your changes. Each line shows its default, and mistakes are marked as you type. "add setting" lists every setting and key.

```ini
[editor]
font = Georgia

[spell]
languages = en_GB, de_DE

[keys]
note.daily = ctrl+shift+d
```

Keys work where their command does: `ctrl+r` in the notes list, `f2` on a tag. A key you give two commands is marked, with which one runs.

For other languages, put a hunspell dictionary (`de_DE.aff` and `de_DE.dic`) in the `dictionaries` folder next to the config.

"choose theme" previews each theme as you move over it. Besides delta's dark and light, there are ports of monkeytype's serika dark, serika, carbon, 8008, olivia, bento and moonlight, and of nord, dracula, gruvbox, catppuccin, rosé pine, tokyo night and everforest. `[colors]` goes on top of the theme:

```ini
[appearance]
theme = nord

[colors]
accent = #ebcb8b
```

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Dictionaries from SCOWL (http://wordlist.aspell.net). Spellchecking by Hunspell.

Portions of this software are copyright © 2024 The FreeType Project (https://freetype.org). All rights reserved.
