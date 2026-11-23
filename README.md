# Omaread

![Omaread searching an EPUB](docs/screenshot.png)

A book reader for [Omarchy](https://omarchy.org). EPUBs, comics and PDF books, read from the keyboard in your theme's colors, and opened again where you stopped.

Omaread is for reading books from start to finish. EPUBs flow in the theme's colors at the desktop's text size, comics (CBZ, CBR, CB7, CBT) turn a page at a time or two side by side, and PDF and comic pages can be tinted to match the theme. It follows the Omarchy theme and language as they change. Markdown and plain text open too. Filling in forms, annotating and printing PDFs are left to a document viewer.

The library remembers every book you've opened and where you stopped reading, so the next time a book opens, it opens at that place. The library lives in `~/.local/state/omaread/library.json`. Omaread never changes the files it reads.

Open a book from the library, with `O`, by dropping it on the window, or from the command line:

```sh
omaread open book.epub
```

Search the whole book with `/`, select words with the mouse and copy them with `Ctrl+C`, follow links inside the book, and read PDFs and comics two pages at a time with `D`. Pages turn with a slide, a paper-like flip, a cover, a fade, or no animation at all; `A` picks the next one and the choice is remembered. A PDF locked with a password asks for it, and never keeps it. The text size follows the desktop's, set in `~/.config/omarchy/shell.toml`.

## Commands

The same reading works without a window, for scripts and coding agents. Each command takes `--json`, and `--password` for a locked PDF.

```sh
omaread info book.epub              # title, author, format, sections and contents
omaread text book.epub --section 3  # the words of one section, or of the whole book
omaread search manual.pdf vec3      # where words appear, section by section
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
| `G` | Go to a page or section |
| `T` | Table of contents |
| `/` `Ctrl+F` | Find in the book |
| `N` `Shift+N` | Next or previous match |
| `Ctrl+C` | Copy the selected words |
| `+` `−` `0` | Larger, smaller, or reset text and pages |
| `W` | Fit pages to the width or the window |
| `D` | One page or two side by side |
| `I` | Color pages like the theme |
| `A` | Change how pages turn |
| `F` | Full screen |
| `Esc` | Back to the library |
| `Enter` `Delete` | Open or forget a book in the library |
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
- **EPUB**: each chapter is laid out as text in the theme's colors and your text size. The book's own colors and fonts are left out, and its images are scaled to fit. Links between chapters, and to places inside them, are followed inside the reader. The table of contents comes from the EPUB 3 nav document, or from the EPUB 2 NCX.
- **Comics**: the images in a ZIP, RAR, 7z or tar archive, in page-number order.
- **Markdown**: one section for each first- and second-level heading, and those headings make up the table of contents.
- **Text**: split into sections of about 20,000 characters, always between paragraphs.

## Translations

The interface is translated into Omarchy's 49 languages, in `src/i18n`. `en.json` lists every string, and the tests check each catalog against it. When strings change, `scripts/i18n-sync.py` and its workflow bring the other languages up to date.

## License

MIT. See [LICENSE](LICENSE).
