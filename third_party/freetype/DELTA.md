FreeType 2.13.3 (https://freetype.org), used under the FreeType License
(see docs/FTL.TXT; the README carries the credit it asks for).

Unchanged, but trimmed: the library sources, headers and CMake build only.
`builds/` keeps just the folders its CMakeLists.txt reads; `docs/` keeps the
two licenses. zlib, bzip2, PNG, HarfBuzz and Brotli are switched off in delta's
CMakeLists.txt, so compressed, PNG-bitmap and shaped fonts aren't supported.
