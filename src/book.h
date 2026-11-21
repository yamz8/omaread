#pragma once

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStringList>

#include <atomic>
#include <memory>

// One entry in a book's table of contents: where it goes and how deep it sits.
// The fragment names a place inside the section, for a book that keeps several
// chapters in one file.
struct TocEntry {
    QString title;
    int section = 0;
    int depth = 0;
    QString fragment;
};

// One place a search found. On a page, rect is where, as fractions of the
// page; in flowing text, occurrence counts the matches before it in its section.
struct SearchHit {
    int section = 0;
    int occurrence = 0;
    QRectF rect;
};

// A link on a PDF page: the area, as fractions of the page, and where it goes,
// another page or a web address.
struct PageLink {
    QRectF rect;
    int page = -1;
    QString url;
};

// A book or document, read whole from disk and never written back. It is
// loaded on a worker thread and then only read; drawing and searching happen
// on other threads too, and only touch what loading left behind.
//
// Every book is a list of sections. A paged book (PDF, comic) has one per
// page, drawn as an image. A flowing book (EPUB, Markdown, text) has one per
// chapter, as text the window lays out in the theme's colors.
class Book {
public:
    enum class Kind { Pdf, Epub, Comic, Markdown, Text };
    enum class Error { None, Missing, Unreadable, Unsupported, Locked, WrongPassword, Empty };

    ~Book();

    // Reads the file at path, as the book the backend calls id, with the
    // password a locked PDF needs. On failure returns null and sets error.
    static std::shared_ptr<Book> load(const QString &path, int id, Error *error, const QString &password = {});

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
    QString kindName() const;

    // The section a link inside the book leads to, or -1, and the place in it.
    int sectionForLink(const QString &link, QString *fragment = nullptr) const;
    // How many characters of a flowing section's text come before the place a
    // fragment names, or -1 when it names none.
    int fragmentOffset(int section, const QString &fragment) const;

    // A page drawn to fit size. With a tint, white turns into background and
    // black into foreground, so a page reads like the theme.
    QImage renderPage(int page, const QSize &size, const QColor &background = {}, const QColor &foreground = {}) const;
    // An image an EPUB chapter shows, by its path inside the archive.
    QImage resource(const QString &path) const;
    // A section's words, as plain text: a PDF page's, or a chapter's without markup.
    QString plainText(int section) const;
    // The words inside an area of a PDF page, given as fractions of the page.
    QString pageText(int page, const QRectF &area) const;
    QList<PageLink> pageLinks(int page) const;
    // Every place the words appear, ignoring case, in reading order. Stops
    // early, with what it found so far, when cancel turns true.
    QList<SearchHit> search(const QString &words, const std::atomic_bool *cancel = nullptr) const;

    // A chapter's HTML with images no wider than width, keeping their shape.
    static QString fitImages(const QString &html, int width);
    // The text of a counted URL for an image the book holds, for <img src>.
    static QString resourceUrl(int bookId, const QString &path);

private:
    Book() = default;
    bool loadPdf(Error *error, const QString &password);
    bool loadEpub(Error *error);
    bool loadComic(Error *error);
    bool loadMarkdown(Error *error);
    bool loadText(Error *error);

    QByteArray m_password;
    // Comic pages, as their encoded bytes, and an EPUB's images.
    QList<QByteArray> m_pages;
    QHash<QString, QByteArray> m_resources;
    // Poppler isn't safe to use from two threads at once.
    mutable QMutex m_pdfMutex;
};
