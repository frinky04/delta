# delta

A keyboard-driven markdown note app, modelled on a code editor. Notes are plain `.md` files.

## Notes and spaces

Notes live in `~/Documents/delta`. Each folder there is a **space**, and each `.md` file in it is a note.

- `ctrl+n` new note
- `alt+d` today's daily note

Notes save as you type. Deleted notes go to the trash; "restore deleted note" brings one back.

## Palette

`ctrl+p` lists every command, with its shortcut.

`ctrl+o` finds a note in the current space; `tab` widens it to every space, and `ctrl+alt+o` starts there. `work/plan` looks in one space. Notes are listed by name; `ctrl+r` switches to most recent first. Type a name that doesn't exist to create it. `ctrl+tab` flips back to the note before.

A leading character switches what the palette searches, and backspace goes back to commands:

- `.` notes
- `/` spaces
- `@` headings in this note
- `:` line number
- `?` text in this space (`tab` for every space)
- `[` notes that link here

`f1` shows this page.

## Writing

Markdown symbols stay visible, dimmed, with the formatting applied. `ctrl+e` toggles reader mode, which hides them.

`[[note]]` and `[[space/note]]` link to notes. Following a link to a missing note creates it. `alt+left` and `alt+right` go back and forward.

Find (`ctrl+f`), go to line (`ctrl+g`) and rename (`f2`) open in the status line. `esc` closes them.

## Spelling

Misspelt words are underlined, and the likeliest fix shows in the status line.

- `alt+.` apply the fix
- `ctrl+.` go through the note word by word

Code, links and names aren't checked. US and UK English are built in.

## Images

Paste or drop an image to add it. Drag its right edge to resize it. Images on the same line form a row. Double-click one to view it full size.

Images are stored in the notes folder, once each, and linked with standard markdown.

## Config

`ctrl+,` opens the config. It holds only your changes. Each line shows what its setting does and its default, and mistakes are marked as you type. "add setting" lists every setting and key binding.

```ini
[editor]
font = Georgia

[spell]
languages = en_GB, de_DE

[keys]
note.daily = ctrl+shift+d
```

For other languages, put a hunspell dictionary (`de_DE.aff` and `de_DE.dic`) in the `dictionaries` folder next to the config.

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Dictionaries from SCOWL (http://wordlist.aspell.net). Spellchecking by Hunspell.
