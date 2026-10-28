# MicroTeX

LaTeX math renderer, from https://github.com/NanoMichael/MicroTeX at commit
0e3707f (MIT, see LICENSE). Only the core and the Qt backend are kept.

Local changes:

- `src/latex.cpp`: a string from `asprintf` was released with `delete`; it is
  now released with `free`.
- `src/platform/qt/graphic_qt.cpp`: text is drawn as outlines at
pixel sizes. The upstream backend draws glyphs about one point high through a
scaled painter, which crashes Qt 6.10's glyph cache (FreeType error 62), and it
treats MicroTeX's pixel sizes as points, which draws every glyph a third too
large at 96 DPI.
- `src/utils/indexed_arr.h`: the binary search started one row past the end of
  the table, which read past a font table for every accent.
- `src/box/box_factory.cpp`, `src/atom/atom_matrix.cpp`: a stretched delimiter
  or arrow, or a row of `\hdotsfor` dots, is refused past `MAX_REPEATS` pieces.
  A length in the source could ask for millions of them, or with an infinite
  size loop for ever.
