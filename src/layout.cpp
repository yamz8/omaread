#include "layout.h"

#include <QAbstractTextDocumentLayout>
#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextList>

#include <algorithm>
#include <cmath>
#include <vector>

#include "book.h"
#include "stylesheet.h"

namespace Layout {

namespace {

// Where typeset() records how it set the chapter, and which paragraphs it gave
// a gap, so a new text size or setting changes only what it set itself.
constexpr int paragraphGapProperty = QTextFormat::UserProperty + 1;
constexpr int paragraphGapSetProperty = QTextFormat::UserProperty + 2;
constexpr int justifyProperty = QTextFormat::UserProperty + 3;

struct Line {
    double top, height;
    int character;
};

} // namespace

void typeset(QTextDocument *text, double lineHeight, double paragraphGap, bool justify, bool markdown) {
    if (!text || text->isEmpty())
        return;
    const qreal height = lineHeight * 100;
    // Already set this way: setting it again would only lay the chapter out twice.
    const QTextBlockFormat first = text->firstBlock().blockFormat();
    if (qFuzzyCompare(first.lineHeight(), height) && first.lineHeightType() == QTextBlockFormat::ProportionalHeight
        && qFuzzyCompare(first.doubleProperty(paragraphGapProperty) + 1, paragraphGap + 1)
        && first.hasProperty(justifyProperty) && first.boolProperty(justifyProperty) == justify)
        return;
    QTextBlockFormat format;
    format.setLineHeight(height, QTextBlockFormat::ProportionalHeight);
    format.setProperty(paragraphGapProperty, paragraphGap);
    format.setProperty(justifyProperty, justify);
    QTextCursor cursor(text);
    cursor.beginEditBlock();
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
    // Markdown's paragraphs arrive with no space between them, where HTML's
    // carry their own margins. Give the bare ones a gap, scaled to the text
    // size, but leave headings, lists and code as the document laid them out.
    // Plain text is left alone: a hard-wrapped file has a block for every line.
    for (QTextBlock block = text->begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat own = block.blockFormat();
        const bool code = own.hasProperty(QTextFormat::BlockCodeFence) || own.nonBreakableLines();
        bool changed = false;
        const bool bare = markdown && own.headingLevel() == 0 && !block.textList() && !code && own.topMargin() == 0
                          && (own.bottomMargin() == 0 || own.boolProperty(paragraphGapSetProperty));
        if (bare) {
            own.setBottomMargin(paragraphGap);
            own.setProperty(paragraphGapSetProperty, true);
            changed = true;
        }
        // Justified or ragged is the reader's choice for running text, as it
        // is on any e-reader; text the book centers or sets right stays so.
        const Qt::Alignment horizontal = own.alignment() & Qt::AlignHorizontal_Mask;
        const bool running = own.headingLevel() == 0 && !code
                             && (!own.hasProperty(QTextFormat::BlockAlignment) || horizontal == Qt::AlignLeft
                                 || horizontal == Qt::AlignLeading || horizontal == Qt::AlignJustify);
        if (running) {
            own.setAlignment(justify ? Qt::AlignJustify : Qt::AlignLeading);
            changed = true;
        }
        if (changed)
            QTextCursor(block).setBlockFormat(own);
    }
    cursor.endEditBlock();
}

Pages split(QTextDocument &document, double view) {
    std::vector<Line> lines;
    auto *layout = document.documentLayout();
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QRectF box = layout->blockBoundingRect(block);
        const QTextLayout *lines_ = block.layout();
        for (int i = 0; lines_ && i < lines_->lineCount(); ++i) {
            const QTextLine line = lines_->lineAt(i);
            lines.push_back({box.top() + line.y(), line.height(), block.position() + line.textStart()});
        }
    }
    const double textHeight = document.size().height();
    const double content = textHeight + 2 * textMargin;

    // Pages as a book has them: each holds the whole lines that fit
    // between a margin at its top and one at its bottom, and the next
    // starts with the first line that didn't. A line taller than a page,
    // a picture, has a page to itself. In the view, a page starting at
    // the text's y is shown with the view scrolled to y, which puts that
    // line one margin below the view's top.
    const double room = std::max(40.0, view - 2.0 * textMargin);
    QList<double> starts{0};
    QList<int> characters{lines.empty() ? 0 : lines.front().character};
    size_t first = 0;
    while (first < lines.size()) {
        const double top = lines[first].top, edge = top + room;
        size_t next = first + 1;
        while (next < lines.size() && lines[next].top + lines[next].height <= edge + 0.5)
            ++next;
        // Lines sharing a row, as a picture beside text can, go together.
        while (next < lines.size() && lines[next].top < lines[next - 1].top + 0.5)
            ++next;
        if (next >= lines.size())
            break;
        starts << lines[next].top;
        characters << lines[next].character;
        first = next;
    }
    // The last page is shown whole, with room below it to the view's foot.
    return Pages{starts, characters, std::max(content, starts.last() + view)};
}

QList<Pages> paginate(const Book &book, const Page &page, const std::atomic_bool *cancel) {
    QList<Pages> pages;
    if (book.paged() || page.width <= 0 || page.height <= 0 || page.pixelSize <= 0)
        return pages;
    QFont font(page.family);
    font.setPixelSize(page.pixelSize);
    const double view = page.height;
    const bool markdown = book.kind == Book::Kind::Markdown;

    for (int section = 0; section < book.sections.size(); ++section) {
        if (cancel && *cancel)
            return {};
        // The chapter as the window's text view holds it.
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setDefaultFont(font);
        QTextOption option = document.defaultTextOption();
        option.setWrapMode(QTextOption::WordWrap);
        document.setDefaultTextOption(option);
        const QString &text = book.sections[section];
        switch (book.kind) {
        case Book::Kind::Epub:
            document.setHtml(Styling::sized(Book::fitImages(text, page.width), page.pixelSize));
            break;
        case Book::Kind::Markdown: document.setMarkdown(text); break;
        default: document.setPlainText(text); break;
        }
        typeset(&document, page.lineHeight, std::round(page.pixelSize * 0.75), page.justify, markdown);
        document.setTextWidth(page.width);

        pages << split(document, view);
    }
    return pages;
}

} // namespace Layout
