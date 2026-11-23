---
name: omaread
description: |
  Read books and documents with Omaread: PDFs, EPUBs, comic archives,
  Markdown and text. Use for ANY request to read, summarize, quote from or
  search a PDF or EPUB, list a book's chapters, find where something is
  said in a document, or gather the passages and notes the person highlighted. Omaread only reads; it never changes a file.
---

# Omaread

Omaread is the Omarchy book and document reader. Its commands read a book the
way the window does and print what is in it. No command needs a display, and
none changes the file.

## Commands

- `omaread info <file> --json`: title, author, format (pdf, epub, comic,
  markdown, text), the number of sections, and the table of contents with the
  section each entry starts in.
- `omaread text <file> --section N`: the words of one section, counting from 1.
  A PDF's sections are its pages; an EPUB's are its chapter files; Markdown is
  cut at its first- and second-level headings; text at about 20,000
  characters. Leave out `--section` for the whole book.
- `omaread search <file> <words> --json`: every section the words appear in,
  ignoring case, and how often. A match is a word that begins with the words.
- `omaread highlights <file>`: the passages the person highlighted in the
  book and the notes they wrote, as Markdown under each chapter's name; with
  `--json`, each one's section, chapter, text, note and color.
- `omaread open <file>`: opens the book in the window, for the person to read.

A locked PDF needs `--password`. Ask the person for it; never guess or try
passwords.

## Working with a long book

Run `info` first and read the contents. Then use `search` to find the
sections that matter and `text --section N` to read only those, rather than
printing a whole book at once. When you quote, say which section or page the
words come from. Comics are pictures; `text` has nothing to print for them.
