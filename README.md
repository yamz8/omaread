# Omaread

![Omaread reading an EPUB](docs/screenshot.png)

A book and document reader for [Omarchy](https://omarchy.org). It opens PDFs, EPUBs, comic archives (CBZ, CBR, CB7, CBT), Markdown and plain text, and it's driven from the keyboard. It follows the Omarchy theme and language as they change. Books flow in the theme's colors, and PDF and comic pages can be tinted to match it.

The library remembers every book you've opened and where you stopped reading, so the next time a book opens, it opens at that place. The library lives in `~/.local/state/omaread/library.json`. Omaread never changes the files it reads.

Open a book from the library, with `O`, by dropping it on the window, or from the command line:

```sh
omaread book.epub
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
| `T` | Table of contents |
| `+` `−` `0` | Larger, smaller, or reset text and pages |
| `W` | Fit pages to the width or the window |
| `I` | Color pages like the theme |
| `F` | Full screen |
| `Esc` | Back to the library |
| `Enter` `Delete` | Open or forget a book in the library |
| `?` | Show or hide the shortcuts |
| `Q` | Quit |

## Building

You need Qt 6 (base, declarative, svg, wayland, and Qt Pdf from `qt6-webengine`), libarchive, and CMake.

```sh
bin/build    # cmake into build/
bin/test     # build, then run the tests offscreen
cmake --install build --prefix ~/.local   # put it in the app launcher
```

## How it reads

- **PDF**: drawn page by page with Qt Pdf, along with its outline.
- **EPUB**: each chapter is laid out as text in the theme's colors and your text size. The book's own colors and fonts are left out, and its images are scaled to fit. Links between chapters are followed inside the reader. The table of contents comes from the EPUB 3 nav document, or from the EPUB 2 NCX.
- **Comics**: the images in a ZIP, RAR, 7z or tar archive, in page-number order.
- **Markdown**: one section for each first- and second-level heading, and those headings make up the table of contents.
- **Text**: split into sections of about 20,000 characters, always between paragraphs.

## License

MIT. See [LICENSE](LICENSE).
