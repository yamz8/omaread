#pragma once

#include <QList>
#include <QtGlobal>
#include <QString>

#include <atomic>

class Book;
class QTextDocument;

// How a flowing book's text is set, shared by the window, which lays out the
// chapter on view, and by the page count, which lays out every chapter the
// same way in the background.
namespace Layout {

// Sets a chapter's lines: their height as a multiple of the text size, the
// gap after a Markdown paragraph, and justified or ragged running text.
void typeset(QTextDocument *document, double lineHeight, double paragraphGap, bool justify, bool markdown);

// What the window shows a chapter in, as the page count needs it.
struct Page {
    int width = 0;          // the text column
    int height = 0;         // the view the column scrolls in
    QString family;
    int pixelSize = 0;
    double lineHeight = 1.4;
    bool justify = false;
    bool operator==(const Page &o) const {
        return width == o.width && height == o.height && family == o.family && pixelSize == o.pixelSize
               && qFuzzyCompare(lineHeight, o.lineHeight) && justify == o.justify;
    }
};

// The margin above and below a chapter's text in the view, and how far above
// the line a page turn cut off the next page starts. The window uses the same.
constexpr int textMargin = 40;
constexpr int turnPad = 24;

// A section's pages, as the window shows them: where each page starts, as a
// scroll offset in the view and as the first character on it, and how tall
// the section is laid out.
struct Pages {
    QList<double> tops;
    QList<int> characters;
    double height = 0;
};

// Every section's pages. A page turn starts the next page at the line the
// last one cut off, so these are the places Space stops at. Empty when
// cancelled.
QList<Pages> paginate(const Book &book, const Page &page, const std::atomic_bool *cancel = nullptr);

} // namespace Layout
