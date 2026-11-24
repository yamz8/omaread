#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

// A book's own styling, carried into Qt's rich text. Qt's HTML understands
// little of the CSS a carefully made EPUB is set with: no first-line indents,
// no sizes relative to the text, no "the paragraph after a paragraph", no
// attribute selectors. So the cascade runs here, over each chapter's XHTML,
// and every element gets the result as an inline style Qt does understand.
//
// Only what shapes the text is kept: indents, margins, alignment, italic and
// bold, small capitals and case, relative sizes, superscripts, and elements
// a book hides. Colors, backgrounds, line height and the typeface stay the
// reader's, so every book follows the theme and the reading settings.
namespace Styling {

struct Declaration {
    QString property;
    QString value;
    bool important = false;
};

struct Selector;

struct Rule {
    QList<std::shared_ptr<Selector>> selectors;
    QList<Declaration> declarations;
    int order = 0;
};

// One stylesheet's rules, in the order they appear.
class Sheet {
public:
    static Sheet parse(const QString &css);
    QList<Rule> rules;
};

// The chapter's body as HTML for Qt, its elements styled by the sheets (in
// cascade order) and their own style attributes. Lengths are written in
// "qe", a unit of the reading text size that sized() turns into pixels, so
// the book's spacing grows and shrinks with the text. Returns a null string
// when the chapter isn't well-formed XML, for the caller to read another way.
//
// With notes given, the footnotes and endnotes in the chapter are also
// written there by their id, each styled like the text and without the link
// back to where it's referred to, so the reader can show one in place.
QString apply(const QByteArray &xhtml, const QList<const Sheet *> &sheets, QHash<QString, QString> *notes = nullptr);

// Sheets made ready to apply, their rules sorted by what they can match, so
// each element is tried against only those. A book's chapters mostly share
// their sheets, and so share these.
class Rules;
std::shared_ptr<const Rules> compile(const QList<const Sheet *> &sheets);
QString apply(const QByteArray &xhtml, const Rules &rules, QHash<QString, QString> *notes = nullptr);

// The stylesheets a chapter links to and carries, as paths inside the book
// relative to the chapter, and the text of its <style> elements.
void linkedSheets(const QByteArray &xhtml, QStringList *hrefs, QStringList *inline_);

// Lengths in qe, as apply() writes them, in pixels for a text size.
QString sized(const QString &html, int pixelSize);

} // namespace Styling
