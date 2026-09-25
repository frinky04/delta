# AGENTS.md

delta is a markdown note app that feels like a code editor: keyboard-first, fast, quiet. C++20, Dear ImGui, GLFW; `third_party/` is vendored.

## Feel

- Every action is a command (`ctrl+p`) with a rebindable id (`note.rename`); the mouse is a shortcut to them.
- A command does one kind of thing and has a `Scope` (`src/app.h`), where its key works. A key goes to the innermost scope that binds it, so `f2` is `tag.rename` on a tag and `note.rename` elsewhere; never branch one command on what's under the caret. A palette mode's commands (`ctrl+r`, `tab`) show their keys beside its prompt, and picked from the root they open the mode first. Moving within a tool (arrows, enter, esc, tab between fields) isn't a command.
- One route per job. The palette's root is commands; every other mode is a leading character, entered by its own key and left with backspace. Scoped modes widen from this space to every space with `tab`. `#` is tags; a row can fill the query (`PaletteItem::fill`) instead of closing it.
- The note stays in view. Tools that act on it (find, rename, go to line, fix spelling) open in the status line; choices and confirmations open in the palette. No dialogs, no extra windows.
- Anything completed as you type (`#tag`, `[[link`) is a `Completer` (`src/editor.h`); the editor owns the list and its keys.
- Markdown stays visible, styled in place; reader mode hides the symbols. The palette and status bar are set in mono.
- Notes are plain `.md` files in plain folders, and nothing touches the network unless the user pastes a URL.
- The config is one ini file, edited in delta, holding only what the user changed; defaults and problems show inline beside it.
- Motion is subtle, and idle means asleep: render only while something moves.

## Voice

Text delta puts on screen is lowercase fragments, like editor chrome: `open a note first; the config stays where it is`.

- Lowercase start, acronyms too (`url`). No full stop; join clauses with `;`.
- The user's text keeps its case: note and space names, file names, setting keys.
- OS error text goes through `plain_msg()` (`src/util.h`).
- Prose (README, code comments) is normal sentence case, but quotes commands by their exact title.

`test_ui_voice` checks inline literals only; messages built elsewhere, like `Store`'s errors, are on you.

## README

`README.md` is embedded at build time as the in-app manual ("open manual"), so it's for users only, and in markdown delta renders: no tables, no `#anchor` links.

## Build and check

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/delta_tests.exe
```

To see a UI change: `build/delta.exe --sandbox --run a,b --screenshot out.png --size 1200x800` renders and exits. `--sandbox` is a first launch in a temporary folder, off the user's notes; `--sandbox <dir>` keeps the folder, so running it again is a second launch. `--run` takes command ids, `open:<note>` and `palette:<text>`.
