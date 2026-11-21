#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>

class QPdfDocument;

// One entry in a book's table of contents: where it goes and how deep it sits.
struct TocEntry {
    QString title;
    int section = 0;
    int depth = 0;
};

// A book or document, read whole from disk and never written back. It is
// loaded on a worker thread and then only read, except for rendering, which
// the image provider does from its own threads under the book's mutex.
//
// Every book is a list of sections. A paged book (PDF, comic) has one per
// page, drawn as an image. A flowing book (EPUB, Markdown, text) has one per
// chapter, as text the window lays out in the theme's colors.
class Book {
public:
    enum class Kind { Pdf, Epub, Comic, Markdown, Text };
    enum class Error { None, Missing, Unreadable, Unsupported, Locked, Empty };

    ~Book();

    // Reads the file at path, as the book the backend calls id. On failure
    // returns null and sets error.
    static std::shared_ptr<Book> load(const QString &path, int id, Error *error);

    Kind kind = Kind::Text;
    QString path;
    QString title;
    QString author;
    QList<TocEntry> toc;
    // The width over height of every section: pages for a paged book, and 0
    // for a flowing one.
    QList<double> aspects;
    // A flowing book's sections: HTML for an EPUB, Markdown, or plain text.
    QStringList sections;
    // Given by the backend, so an image request for a book that has since been
    // replaced is told apart from one for this book.
    int id = 0;

    bool paged() const { return kind == Kind::Pdf || kind == Kind::Comic; }
    int sectionCount() const { return paged() ? int(aspects.size()) : int(sections.size()); }
    // The section a link inside the book leads to, or -1.
    int sectionForLink(const QString &link) const;

    // A page drawn to fit size. With a tint, white turns into background and
    // black into foreground, so a page reads like the theme.
    QImage renderPage(int page, const QSize &size, const QColor &background = {}, const QColor &foreground = {}) const;
    // An image an EPUB chapter shows, by its path inside the archive.
    QImage resource(const QString &path) const;

    // A chapter's HTML with images no wider than width, keeping their shape.
    static QString fitImages(const QString &html, int width);
    // The text of a counted URL for an image the book holds, for <img src>.
    static QString resourceUrl(int bookId, const QString &path);

private:
    Book() = default;
    bool loadPdf(Error *error);
    bool loadEpub(Error *error);
    bool loadComic(Error *error);
    bool loadMarkdown(Error *error);
    bool loadText(Error *error);

    std::unique_ptr<QPdfDocument> m_pdf;
    // Comic pages, as their encoded bytes, and an EPUB's images.
    QList<QByteArray> m_pages;
    QHash<QString, QByteArray> m_resources;
    // Archive paths of an EPUB's chapters, for following links between them.
    QStringList m_chapterPaths;
    mutable QMutex m_mutex;
};
