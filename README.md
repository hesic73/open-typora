# Open Typora

A Markdown editor in the manner of Typora, written in C++20 with Qt 6 Widgets.
The document is shown as it reads. Put the caret into a construct and only its
Markdown appears so it can be edited: `**bold**` shows its asterisks while the
caret is in it, a link shows its address, and a formula shows its LaTeX with a
live preview. Move away and it is formatted again.

There is no web view: text is laid out with Qt's text engine, and math is
typeset with MicroTeX.

## Build and run

Needs Qt 6.5 or newer (Widgets, Svg, Network, PrintSupport), CMake, Ninja and a
C++20 compiler.

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/open-typora [file.md | folder]
```

For a Qt that is not installed system-wide, add
`-DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.2/gcc_64` (the folder holding `lib/cmake/Qt6`).
`cmake --install build-release --prefix ~/.local` installs the program, its
desktop entry and the math fonts.

## What it does

- **Live preview editing.** Headings, emphasis, strike, ==highlight==, code,
  links, images, footnote references, HTML tags such as `<u>` `<sup>` `<br>`
  `<img>`, entities and escapes are formatted in place. The block with the
  caret reveals the syntax of the spans the caret touches, and nothing else.
  As in Typora, heading `#` marks and list and quote markers are never shown:
  use Ctrl+1–6 for heading levels, and Backspace at the start of a heading,
  list item or quote turns it back into a paragraph.
- **Typing shortcuts.** `# ` makes a heading, `- `, `* `, `1. ` a list,
  `- [ ] ` a task, `> ` a quote, `---` a rule. ```` ```lang ```` or `$$`
  followed by Enter opens a code or math block; `| a | b |` followed by Enter
  makes a table.
- **Lists.** Enter continues a list, counting up ordered items; Enter on an
  empty item leaves the list (or moves a nested item out a level). Tab and
  Shift+Tab nest and un-nest. Task boxes toggle with a click.
- **Tables** are edited as a grid: Tab and Shift+Tab move between cells, Enter
  goes to the next row (adding one at the end), Ctrl+Enter inserts a row. The
  context menu adds and deletes rows and columns, sets alignment, and tidies
  the source. A `|` typed in a cell is escaped.
- **Code blocks** are coloured for C, C++, Python, JavaScript/TypeScript, Rust,
  Go, Java, shell, JSON, YAML, HTML/XML, CSS, SQL, diff and more. Enter keeps
  the line's indentation; Tab indents.
- **Math.** `$...$` inline and `$$...$$` blocks, typeset with MicroTeX. A dollar
  amount such as "$5 and $10" stays text.
- **Pictures** from files (relative to the document), from the web (fetched in
  the background) and `data:` URLs. Pasting a picture saves it to `assets/`
  next to the document; dropping or pasting picture files links them.
- **Context menu**, as in Typora: Cut, Copy and Paste, Copy / Paste As (Markdown,
  plain text), a row of formatting buttons (strong, emphasis, code, link, quote,
  lists, tasks), then Paragraph (heading levels) or, inside a table, Table (add,
  move and delete rows and columns, copy, prettify, delete), or, in a code or
  math block, a way back to a paragraph; and Insert (image, footnote, link
  reference, rule, table, code, math, table of contents, YAML front matter,
  paragraph before or after). Over a link it opens or copies the link; with a
  selection it can search for it.
- **Copy** puts formatted text on the clipboard alongside the Markdown, so it
  pastes formatted into other applications; Copy as Markdown gives only the
  Markdown. `[TOC]` shows the document's headings as links.
- **Source code mode** (Ctrl+/) shows the plain Markdown, coloured, and keeps
  the caret's place when switching.
- **Sidebar** with the outline (following the caret; click to jump) and a file
  tree of the open folder (Ctrl+Shift+L to show or hide).
- **Find and replace** (Ctrl+F, Ctrl+H) with case, whole-word and regular
  expression options.
- **Focus mode** (F8) dims everything but the current block; **typewriter
  mode** (F9) keeps the caret line in the middle of the window.
- **Themes**: Github, Newsprint, Night, Pixyll and Whitey, after Typora's own.
  Zoom with Ctrl+Shift+= / Ctrl+Shift+- or Ctrl+wheel.
- **Export** to PDF (laid out as on screen) and to a standalone HTML page
  (formulas as inline SVG); printing.
- Word, character and line counts in the footer (with the selection's count while selecting), autosave, recent files, and
  reloading when the file changes on disk.

Files are read and written as UTF-8. A file opened and saved without edits
comes back the same, apart from line endings (written as `\n`) and blank lines
at its start and end.

## Shortcuts

| Keys | Action |
| --- | --- |
| Ctrl+1 … Ctrl+6, Ctrl+0 | Heading level, paragraph |
| Ctrl+= / Ctrl+- | Raise / lower heading level |
| Ctrl+B, Ctrl+I, Ctrl+U | Strong, emphasis, underline |
| Ctrl+Shift+\`, Alt+Shift+5 | Inline code, strike |
| Ctrl+Shift+H, Ctrl+Shift+E | Highlight, inline math |
| Ctrl+K, Ctrl+Shift+I | Link, image |
| Ctrl+\\ | Clear formatting of the selection |
| Ctrl+T | Table |
| Ctrl+Shift+K, Ctrl+Shift+M | Code block, math block |
| Ctrl+Shift+Q | Quote |
| Ctrl+Shift+[, Ctrl+Shift+], Ctrl+Shift+X | Ordered, bullet, task list |
| Ctrl+[ / Ctrl+] | Outdent / indent list item |
| Ctrl+Enter | New paragraph below (new row in a table) |
| Alt+arrows, Ctrl+Shift+Backspace | Move a table row or column, delete a row |
| Shift+Enter | Line break within a paragraph |
| Ctrl+/ | Source code mode |
| Ctrl+Shift+L | Sidebar |
| F8, F9, F11 | Focus mode, typewriter mode, full screen |
| Ctrl+click | Open a link |

## Tests

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests run offscreen. `tst_document` covers block splitting, serialisation and
inline parsing; `tst_editor` drives the editor with key events (lists, tables,
code and math blocks, undo, selection, find and replace).

## Layout

```
src/editor   document model, inline parser, block renderer, the editor widget, themes
src/render   math (MicroTeX), code colouring, inline objects, picture loading
src/app      main window, sidebar, find bar, source mode, export
tests        unit and behaviour tests
third_party  MicroTeX (MIT, with local fixes; see its README) and tinyxml2 (zlib)
```

The editor keeps the document as a list of blocks, each holding the Markdown
it was read from (`Document.h`). Each block is laid out in its own
`QTextDocument` with a map between what is shown and the source
(`BlockRenderer.h`), which is how the caret moves through hidden syntax and how
clicks land in the right place in the source.
