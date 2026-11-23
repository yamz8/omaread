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

QList<Pages> paginate(const Book &book, const Page &page, const std::atomic_bool *cancel) {
    QList<Pages> pages;
    if (book.paged() || page.width <= 0 || page.height <= 0 || page.pixelSize <= 0)
        return pages;
    QFont font(page.family);
    font.setPixelSize(page.pixelSize);
    const double view = page.height;
    // As the window's screenStep(), for a page whose bottom edge cuts no line.
    const double screenStep = std::max(40.0, view - 3.0 * page.pixelSize);
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

        // Space's walk through the chapter: the next page starts where the
        // line the bottom edge cuts begins, less the turn's pad, unless no line
        // is cut or the cut one is too tall to hide (a picture).
        // The first character on the page starting at a scroll offset: on the
        // first line whose top shows below the page's top margin.
        auto firstCharacter = [&](double offset) {
            const double y = offset - textMargin;
            const auto it = std::lower_bound(lines.begin(), lines.end(), y - 1,
                                             [](const Line &line, double at) { return line.top < at; });
            return it == lines.end() ? (lines.empty() ? 0 : lines.back().character) : it->character;
        };
        QList<double> starts{0};
        double top = 0;
        while (top + view < content - 2) {
            const double edge = top + view - textMargin;   // in the text's own coordinates
            double cover = 0;
            if (edge >= 0 && edge <= textHeight) {
                const auto it = std::upper_bound(lines.begin(), lines.end(), edge,
                                                 [](double y, const Line &line) { return y < line.top; });
                if (it != lines.begin()) {
                    const Line &line = *std::prev(it);
                    if (line.top < edge && line.top + line.height > edge)
                        cover = edge - line.top;
                }
            }
            if (cover >= view * 0.3)
                cover = 0;
            const double cut = view - cover;
            double next = cut > turnPad ? top + cut - turnPad : top + screenStep;
            next = std::min(next, content - view);
            if (next <= top + 1)
                break;
            starts << next;
            top = next;
        }
        Pages section_{starts, {}, content};
        for (const double start : std::as_const(starts))
            section_.characters << firstCharacter(start);
        pages << section_;
    }
    return pages;
}

} // namespace Layout
