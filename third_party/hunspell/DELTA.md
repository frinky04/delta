Hunspell 1.7.2 (https://github.com/hunspell/hunspell), the library sources
only, used under the MPL 1.1 (see COPYING.MPL).

One change: `src/filemgr.hxx` / `src/filemgr.cxx` can read a file from memory
(`hunspell_memory_file`), so delta can load dictionaries embedded in the exe,
or read by itself (no narrow-path trouble with non-ASCII folders on Windows).
Marked `delta:` in the source.
