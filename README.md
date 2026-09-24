# delta

A keyboard-driven markdown note app, modelled on a code editor. Notes are plain `.md` files.

## Notes and spaces

Notes live in `~/Documents/delta`. Each folder there is a **space**, and each `.md` file in it is a note.

- `ctrl+n` new note
- `alt+d` today's daily note

Notes save as you type. Deleted notes go to the trash; "restore deleted note" brings one back.

If a note changes on disk while you have edits not yet saved, delta stops saving it until you choose: `ctrl+s` keeps your version, "reload note" takes the file's. Leave the note without choosing and your edits are kept beside it as "name (yours)".

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
- `#` notes by tag (`tab` for every space)

`f1` shows this page.

## Writing

Markdown symbols stay visible, dimmed, with the formatting applied. `ctrl+e` toggles reader mode, which hides them.

`[[note]]` and `[[space/note]]` link to notes. Following a link to a missing note creates it. `alt+left` and `alt+right` go back and forward.

While you type a `[[link]]` or a `#tag`, a list of notes or tags comes up under the caret. Up and down choose one, `enter` takes it, and `esc` puts the list away.

`alt+shift+enter` searches the web for the selected text, in your default browser with its search engine.

Find (`ctrl+f`), go to line (`ctrl+g`) and rename (`f2`) open in the status line. `esc` closes them.

## Tags

A space is where a note lives; tags say what it is, and a note can have as many as it needs. Write `#` and a name anywhere in a note: `#idea`, `#reading-list`, `#project/delta`. Names are letters, digits, `_`, `-` and `/`, with at least one letter, so `#12` stays a number. Tags in code aren't tags, and case doesn't matter: `#Idea` is `#idea`.

`ctrl+t` lists the tags in this space, and how many notes have each; `tab` widens it to every space. Pick a tag to see its notes. Add more to narrow it down: `#rust #idea` lists the notes that have both, and `-#done` leaves out the ones that have that.

Following a tag (`alt+enter`, or `ctrl`+click) lists the notes that have it.

`f2` with the caret on a tag renames it in every note, in every space, after asking first. Renaming it to a tag that already exists merges the two.

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

"choose theme" lists the built-in themes and shows each one as you move over it. Enter keeps it, escape goes back. Besides delta's own dark and light, there are ports of monkeytype's serika dark, serika, carbon, 8008, olivia, bento and moonlight, and of nord, dracula, gruvbox, catppuccin, rosé pine, tokyo night and everforest. Any colour in `[colors]` goes on top of the theme:

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
