# Omaread

![Omaread searching an EPUB](docs/screenshot.png)

A book reader for [Omarchy](https://omarchy.org). EPUBs, comics and PDF books, read from the keyboard in your theme's colors, and opened again where you stopped.

Omaread is for reading books from start to finish. EPUBs flow in the theme's colors at the desktop's text size, comics (CBZ, CBR, CB7, CBT) turn a page at a time or two side by side, and PDF and comic pages can be tinted to match the theme. It follows the Omarchy theme and language as they change. Markdown and plain text open too. Filling in forms, annotating and printing PDFs are left to a document viewer.

Choose words with the mouse to highlight them in one of four colors, write a note on them, copy them, or look them up in a dictionary installed on the computer (`sdcv` or `dictd`). The contents' Notes tab lists every highlight and note in reading order; `C` there copies them all as Markdown, and `omaread highlights` prints them. Highlighting works in EPUB, Markdown and text books.

The footer names the part and chapter you're in, and tells the time left in the chapter at your own reading pace, which it learns as you read; `P` switches it to the time left in the book, the page, or the percent read. Pages are counted as Space turns them, at the window's size and the reading settings, so a book's page count follows them. Progress counts only the book's text, by its words, so title pages, the imprint and the endnotes don't move it.

The library shows your books as covers (`V` switches to a list), most recent first; `S` changes the order to title, author or progress, `Tab` shows only the books you're reading, haven't started or have finished, and `/` searches titles and authors. A book is marked finished when you reach its end, and `M` marks one by hand. Books you put in `~/Books` join the library by themselves. Covers are kept small in `~/.cache/omaread/covers`.

The library remembers every book you've opened and where you stopped reading, so the next time a book opens, it opens at that place. The library lives in `~/.local/state/omaread/library.json`. Omaread never changes the files it reads.

Open a book from the library, with `O`, by dropping it on the window, or from the command line:

```sh
omaread open book.epub
```

Search the whole book with `/`, for words that begin with what you type, and choose a match from a list that shows each one in the words around it, select words with the mouse and copy them with `Ctrl+C`, follow links inside the book, and read PDFs and comics two pages at a time with `D`. Books are set in a serif by default; `S` changes the typeface, text size, line spacing, column width, and ragged or justified lines, for every book. Pages turn with a slide, a paper-like flip, a cover, a fade, or no animation at all; `A` picks the next one and the choice is remembered. A PDF locked with a password asks for it, and never keeps it. The text size follows the desktop's, set in `~/.config/omarchy/shell.toml`.

## Commands

The same reading works without a window, for scripts and coding agents. Each command takes `--json`, and `--password` for a locked PDF.

```sh
omaread info book.epub              # title, author, format, sections and contents
omaread text book.epub --section 3  # the words of one section, or of the whole book
omaread search manual.pdf vec3      # where words appear, section by section
omaread highlights book.epub        # your highlights and notes, as Markdown
omaread skill install               # teach coding agents to use these commands
```

## Keys

| Key | Action |
| --- | --- |
| `O` | Open a book or document |
| `Space` `→` `PgDn` | Next screen or page |
| `Shift+Space` `←` `PgUp` | Previous screen or page |
| `↓` `↑` `J` `K` | Scroll |
| `]` `[` | Next or previous section |
| `Home` `End` | First or last section |
| `G` | Skim the book: `←` `→` a page, `Shift+←` `Shift+→` a chapter, a typed number, `Enter` to go, `Esc` to stay |
| `Tab` `Shift+Tab` `Enter` | Choose a link or note on the page, and open it |
| `Backspace` `Shift+Backspace` | Back to where a jump left, and on again |
| `T` | Contents, with page numbers, and bookmarks (`Tab` switches) |
| `B` | Bookmark the page, or remove its bookmark |
| `H` | Highlight the words chosen with the mouse; click a highlight for its color, a note, copying or looking up |
| `/` `Ctrl+F` | Find in the book |
| `N` `Shift+N` | Next or previous match |
| `Ctrl+C` | Copy the selected words |
| `+` `−` `0` | Larger, smaller, or reset text and pages |
| `W` | Fit pages to the width or the window |
| `D` | One page or two side by side |
| `I` | Color pages like the theme |
| `P` | Show time left in the chapter or book, the page, or the percent read |
| `S` | Reading settings: serif or sans, text size, line spacing, column width, ragged or justified |
| `A` | Change how pages turn |
| `F` | Full screen |
| `Esc` | Back to the library |
| `Enter` `Delete` | Open or forget a book in the library |
| `V` `S` `Tab` `/` | In the library: covers or a list, the order, which books, search |
| `M` | In the library: mark a book finished, or not |
| `?` | Show or hide the shortcuts |
| `Q` | Quit |

## Building

You need Qt 6 (base, declarative, svg, wayland, imageformats), poppler-qt6, libarchive, and CMake, all of which Omarchy already installs. The tests also use `qpdf`.

```sh
bin/build    # cmake into build/
bin/test     # build, then run the tests offscreen
cmake --install build --prefix ~/.local   # put it in the app launcher
```

## How it reads

- **PDF**: drawn page by page with poppler, as Signed draws them, along with its outline, links and words to search and copy.
- **EPUB**: each chapter is laid out as text in the theme's colors and your text size, the way the book itself sets it: first-line indents, small capitals, centered headings and scene breaks, epigraphs, and the indented lines of poems all come from the book's own stylesheet. Its colors, line height and typeface are left out, so every book follows the theme and the reading settings, and its images are scaled to fit. Links between chapters, and to places inside them, are followed inside the reader, and a footnote or endnote opens over the bottom of the page when its number is chosen, so reading never leaves the page; Enter goes to the note itself. The table of contents comes from the EPUB 3 nav document, or from the EPUB 2 NCX.
- **Comics**: the images in a ZIP, RAR, 7z or tar archive, in page-number order.
- **Markdown**: one section for each first- and second-level heading, and those headings make up the table of contents.
- **Text**: split into sections of about 20,000 characters, always between paragraphs.

## Translations

The interface is translated into Omarchy's 49 languages, in `src/i18n`. `en.json` lists every string, and the tests check each catalog against it. When strings change, `scripts/i18n-sync.py` and its workflow bring the other languages up to date.

## License

MIT. See [LICENSE](LICENSE).
