#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QtTest>

#include <QBuffer>
#include <QPainter>
#include <QPdfWriter>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>

#include <archive.h>
#include <archive_entry.h>

#include "backend.h"
#include "book.h"
#include "filepicker.h"
#include "pageprovider.h"
#include "stylesheet.h"
#include "theme.h"
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// The window's item with this objectName. What a Repeater, ListView or Loader
// makes isn't a QObject child of the window, so findChild never finds it; walk
// the items instead. Popups aren't items: find those with findChild.
QQuickItem *item(QQuickItem *from, const QString &name) {
    for (auto *child : from->childItems()) {
        if (child->objectName() == name)
            return child;
        if (auto *found = item(child, name))
            return found;
    }
    return nullptr;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}


// A ZIP with these files, in this order, as EPUBs and comics are.
bool writeZip(const QString &path, const QList<QPair<QString, QByteArray>> &files) {
    archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    if (archive_write_open_filename(a, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    for (const auto &[name, data] : files) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(a, entry);
        archive_write_data(a, data.constData(), data.size());
        archive_entry_free(entry);
    }
    archive_write_close(a);
    archive_write_free(a);
    return true;
}

QByteArray png(int width, int height, QColor color = Qt::red) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

// A three-chapter EPUB: a cover that is an SVG around an image, a chapter
// with an image, a link and inline styles, and a last chapter. EPUB 3 books
// carry a nav document; with ncx set, it is an EPUB 2 book with an NCX instead.
QString writeEpub(const QString &folder, bool ncx = false) {
    const QString path = folder + (ncx ? "/old.epub" : "/book.epub");
    const QByteArray container = R"(<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>)";
    const QByteArray opf = QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>A Small &amp; Quiet Book</dc:title>
    <dc:creator>Ada Writer</dc:creator>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cover" href="text/cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="one" href="text/one.xhtml" media-type="application/xhtml+xml"/>
    <item id="two" href="text/two%20b.xhtml" media-type="application/xhtml+xml"/>
    <item id="pic" href="images/pic.png" media-type="image/png"/>
    <item id="wide" href="images/wide.png" media-type="image/png"/>
  </manifest>
  <spine toc="ncx"><itemref idref="cover"/><itemref idref="one"/><itemref idref="two"/></spine>
</package>)");
    const QByteArray nav = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body>
<nav epub:type="landmarks"><ol><li><a href="text/cover.xhtml">Cover</a></li></ol></nav>
<nav epub:type="toc"><ol>
  <li><a href="text/one.xhtml">One&nbsp;Begins</a>
    <ol><li><a href="text/two%20b.xhtml#later">Two, <em>Later</em></a></li></ol></li>
</ol></nav></body></html>)";
    const QByteArray toc = R"(<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>
  <navPoint id="a"><navLabel><text>First</text></navLabel><content src="text/one.xhtml"/>
    <navPoint id="b"><navLabel><text>Second</text></navLabel><content src="text/two%20b.xhtml"/></navPoint>
  </navPoint>
</navMap></ncx>)";
    const QByteArray cover = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Cover</title><style>p { color: red }</style></head>
<body><svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 40 60">
<image width="40" height="60" xlink:href="../images/pic.png"/></svg></body></html>)";
    const QByteArray one = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title></head>
<body><h1>One</h1><p style="color: red; font-style: italic">It began&nbsp;quietly.</p>
<p><img src="../images/wide.png" alt="A wide picture"/></p>
<p><a href="two%20b.xhtml#later">Skip ahead</a> or <a href="https://example.com/">read more</a>.</p>
<script>alert(1)</script></body></html>)";
    const QByteArray two = R"(<html xmlns="http://www.w3.org/1999/xhtml"><body><h1>Two</h1><p>Before.</p><p id="later">The end.</p></body></html>)";
    QList<QPair<QString, QByteArray>> files = {{"mimetype", "application/epub+zip"},
                                               {"META-INF/container.xml", container},
                                               {"OEBPS/content.opf", ncx ? QByteArray(opf).replace(" properties=\"nav\"", "") : opf},
                                               {"OEBPS/toc.ncx", toc},
                                               {"OEBPS/text/cover.xhtml", cover},
                                               {"OEBPS/text/one.xhtml", one},
                                               {"OEBPS/text/two b.xhtml", two},
                                               {"OEBPS/images/pic.png", png(40, 60)},
                                               {"OEBPS/images/wide.png", png(2000, 500)}};
    if (!ncx)
        files.insert(3, {"OEBPS/nav.xhtml", nav});
    return writeZip(path, files) ? path : QString();
}

// A PDF of A5 pages, each saying its number.
QString writePdf(const QString &folder, int pages) {
    const QString path = folder + "/paper.pdf";
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A5));
    writer.setTitle("Paper");
    QPainter painter(&writer);
    for (int page = 0; page < pages; ++page) {
        if (page > 0)
            writer.newPage();
        painter.setFont(QFont("sans", 40));
        painter.drawText(QRect(0, 0, writer.width(), writer.height() / 2), Qt::AlignCenter, QString::number(page + 1));
    }
    painter.end();
    return path;
}

// A two-chapter EPUB in Standard Ebooks' shape: a chapter that refers to an
// endnote, and the endnotes, each with a link back.
QString writeNotedEpub(const QString &folder) {
    const QString path = folder + "/noted.epub";
    const QByteArray chapter = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>One</title></head>"
        "<body><section id=\"one\" epub:type=\"chapter\"><p>The serfs were freed"
        "<a href=\"endnotes.xhtml#note-1\" id=\"noteref-1\" epub:type=\"noteref\">1</a> that year.</p></section></body></html>";
    const QByteArray endnotes = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Notes</title></head>"
        "<body><section id=\"endnotes\" epub:type=\"endnotes\"><ol>"
        "<li id=\"note-1\" epub:type=\"endnote footnote\"><p>The emancipation of 1861 is <i>meant</i>. "
        "<a href=\"one.xhtml#noteref-1\" role=\"doc-backlink\" epub:type=\"backlink\">back</a></p></li>"
        "</ol></section></body></html>";
    const QByteArray opf = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:title>Noted</dc:title></metadata><manifest>"
        "<item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"notes\" href=\"endnotes.xhtml\" media-type=\"application/xhtml+xml\"/></manifest>"
        "<spine><itemref idref=\"one\"/><itemref idref=\"notes\"/></spine></package>";
    const QByteArray container = "<?xml version=\"1.0\"?><container version=\"1.0\" "
        "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
        "<rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles></container>";
    return writeZip(path, {{"mimetype", "application/epub+zip"}, {"META-INF/container.xml", container},
                           {"content.opf", opf}, {"one.xhtml", chapter}, {"endnotes.xhtml", endnotes}})
        ? path : QString();
}

QString writeText(const QString &path, const QByteArray &text) {
    return write(path, text) ? path : QString();
}

// A copy of a PDF locked with a password, made with qpdf.
QString lockPdf(const QString &source, const QString &password) {
    const QString locked = QFileInfo(source).absolutePath() + "/locked.pdf";
    QProcess qpdf;
    qpdf.start("qpdf", {"--encrypt", password, password, "256", "--", source, locked});
    return qpdf.waitForFinished() && qpdf.exitCode() == 0 ? locked : QString();
}

// Runs the omaread binary as a script would, with no display at all.
struct Run {
    int code = -1;
    QByteArray out, err;
};
Run omaread(const QStringList &arguments) {
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // The commands must not need a display even where the desktop asks for Wayland.
    environment.insert("QT_QPA_PLATFORM", "wayland");
    environment.remove("WAYLAND_DISPLAY");
    environment.remove("DISPLAY");
    process.setProcessEnvironment(environment);
    process.start(OMAREAD_BINARY, arguments);
    process.waitForFinished(30000);
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, process.readAllStandardOutput(),
            process.readAllStandardError()};
}

// Types text into whatever has the window's keyboard.
void typeInto(QWindow *window, const QString &text) {
    for (const QChar c : text)
        QTest::keyClick(window, c.toLatin1());
}

// Counts the times the window asks for a file, in place of the portal.
class FakePicker : public FilePicker {
public:
    int asked = 0;
    void openDocument() override { ++asked; }
};

} // namespace

class OmareadTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void epubReadsChaptersAndContents() {
        QTemporaryDir dir;
        Book::Error error;
        const auto book = Book::load(writeEpub(dir.path()), 7, &error);
        QVERIFY(book);
        QCOMPARE(book->kind, Book::Kind::Epub);
        QCOMPARE(book->title, QString("A Small & Quiet Book"));
        QCOMPARE(book->author, QString("Ada Writer"));
        QCOMPARE(book->sectionCount(), 3);
        QVERIFY(!book->paged());

        // Only the nav document's toc counts, nested as it is.
        QCOMPARE(book->toc.size(), 2);
        QCOMPARE(book->toc[0].title, QString("One Begins"));
        QCOMPARE(book->toc[0].section, 1);
        QCOMPARE(book->toc[0].depth, 0);
        QCOMPARE(book->toc[1].title, QString("Two, Later"));
        QCOMPARE(book->toc[1].section, 2);
        QCOMPARE(book->toc[1].depth, 1);

        // The cover's SVG becomes its image.
        QVERIFY(book->sections[0].contains(Book::resourceUrl(7, "OEBPS/images/pic.png")));
        QVERIFY(!book->sections[0].contains("svg"));
        QVERIFY(!book->sections[0].contains("color"));

        const QString one = book->sections[1];
        QVERIFY(one.contains("It began"));
        QVERIFY(!one.contains("alert"));
        QVERIFY(!one.contains("color"));
        QVERIFY(one.contains("font-style: italic"));
        QVERIFY(one.contains(Book::resourceUrl(7, "OEBPS/images/wide.png") + "\" width=\"2000\" height=\"500\""));
        QVERIFY(one.contains("href=\"https://example.com/\""));
        QCOMPARE(book->sectionForLink("omaread:2"), 2);
        QCOMPARE(book->sectionForLink("omaread:9"), -1);
        // Links and contents entries carry the place inside the chapter.
        QVERIFY(one.contains("href=\"omaread:2#later\""));
        QString fragment;
        QCOMPARE(book->sectionForLink("omaread:2#later", &fragment), 2);
        QCOMPARE(fragment, QString("later"));
        QCOMPARE(book->toc[1].fragment, QString("later"));
        QCOMPARE(book->fragmentOffset(2, "later"), int(QString("Two\nBefore.\n").size()));
        QCOMPARE(book->fragmentOffset(2, "nowhere"), -1);

        // Search counts matches per chapter, in the text the window lays out.
        const QList<SearchHit> hits = book->search("THE END");
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits[0].section, 2);
        // Words that begin with what's asked for: a word's start, never its middle.
        QCOMPARE(book->search("beg").size(), 1);
        QCOMPARE(book->search("egan").size(), 0);
        const QList<SearchHit> began = book->search("began");
        QVERIFY(began[0].context.contains("It began"));
        QCOMPARE(began[0].context.mid(began[0].contextAt, 5), QString("began"));
        QCOMPARE(book->plainText(began[0].section).mid(began[0].offset, 5), QString("began"));
        QVERIFY(book->plainText(1).contains(QRegularExpression("It began\\squietly")));
        QVERIFY(!book->resource("OEBPS/images/pic.png").isNull());

        // Images never come out wider than the text.
        QVERIFY(Book::fitImages(one, 500).contains("width=\"500\" height=\"125\""));
    }

    void booksKeepTheirOwnStyling() {
        const auto sheet = Styling::Sheet::parse("\n"
            "            @charset \"utf-8\";\n"
            "            @namespace epub \"http://www.idpf.org/2007/ops\";\n"
            "            /* A comment { with braces } */\n"
            "            p { margin-top: 0; margin-bottom: 0; text-indent: 1em; color: red; line-height: 3; }\n"
            "            h2 + p, hr + p { text-indent: 0; }\n"
            "            h2 { font-variant: small-caps; text-align: center; font-size: 1.5em; margin: 2em 0 1em; }\n"
            "            .sc { font-variant: all-small-caps; }\n"
            "            [epub|type~=\"z3998:poem\"] p > span { display: block; padding-left: 1em; text-indent: -1em; }\n"
            "            [epub|type~=\"z3998:poem\"] .i1 { padding-left: 2em; }\n"
            "            .offscreen { position: absolute; left: -999em; }\n"
            "            .gone { display: none; }\n"
            "            hr { width: 25%; }\n"
            "            p:first-child { font-style: italic; }\n"
            "            blockquote p:not(.plain) { font-weight: bold; }\n"
            "            p::first-letter { font-size: 3em; }\n"
            "            @media (prefers-color-scheme: dark) { p { font-weight: bold; } }\n"
            "            @supports (font-variant-numeric: oldstyle-nums) { .old { text-transform: uppercase; } }\n"
            "        ");
        const QByteArray xhtml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
            "<head><title>T</title><link rel=\"stylesheet\" href=\"../css/core.css\"/></head>\n"
            "<body><section epub:type=\"chapter\">\n"
            "<h2 class=\"offscreen\">Hidden</h2>\n"
            "<h2>Chapter</h2>\n"
            "<p>First <span class=\"sc\">Words</span> here.</p>\n"
            "<p>Second.</p>\n"
            "<hr/>\n"
            "<p class=\"old\">After the break.</p>\n"
            "<blockquote epub:type=\"z3998:poem\"><p><span>One line,</span><br/><span class=\"i1\">and an indented one.</span></p></blockquote>\n"
            "<blockquote><p class=\"plain\">Plain.</p></blockquote>\n"
            "<p class=\"gone\">Never shown.</p>\n"
            "<p style=\"text-align: right; color: blue\">Signed.</p>\n"
            "</section></body></html>";

        QStringList hrefs, inlineCss;
        Styling::linkedSheets(xhtml, &hrefs, &inlineCss);
        QCOMPARE(hrefs, QStringList({"../css/core.css"}));
        const QString html = Styling::apply(xhtml, {&sheet});
        QVERIFY(!html.isNull());
        // Hidden and undisplayed elements are gone; colors and line height never come.
        QVERIFY(!html.contains("Hidden"));
        QVERIFY(!html.contains("Never shown"));
        QVERIFY(!html.contains("color"));
        QVERIFY(!html.contains("line-height"));
        // A sectioning element becomes a block Qt knows.
        QVERIFY(html.startsWith("<div epub:type=\"chapter\">"));
        // The heading: small caps, centered, larger, with its margins in that size.
        QVERIFY(html.contains("<h2 style=\"font-size: 1.5qe; text-align: center; margin-right: 0qe; margin-left: 0qe; "
                              "font-variant: small-caps; margin-top: 3qe; margin-bottom: 1.5qe\">Chapter</h2>"));
        // The paragraph after the heading isn't indented, the next one is; the first
        // child is italic; ::first-letter and @media never apply.
        QVERIFY(html.contains("<p style=\"text-indent: 0qe; margin-top: 0qe; margin-bottom: 0qe\">First "));
        QVERIFY(html.contains("<p style=\"text-indent: 1qe; margin-top: 0qe; margin-bottom: 0qe\">Second.</p>"));
        QVERIFY(html.contains("<span style=\"font-variant: small-caps\">Words</span>"));
        QVERIFY(html.contains("<hr width=\"25%\" />"));
        // @supports rules apply.
        QVERIFY(html.contains("text-transform: uppercase; margin-top: 0qe; margin-bottom: 0qe\">After the break."));
        // A poem's lines become blocks with hanging indents, as the book sets them.
        // The paragraph's own margins move to its first and last lines, since Qt
        // drops a block's bottom margin when it holds other blocks.
        QVERIFY(html.contains("<p style=\"text-indent: 1qe; font-style: italic; font-weight: bold\">"
                              "<div style=\"text-indent: -1qe; margin-left: 1qe; margin-top: 0qe\">One line,</div>"));
        QVERIFY(html.contains("<div style=\"text-indent: -1qe; margin-left: 2qe; margin-bottom: 0qe\">and an indented one.</div>"));
        // :not() leaves the plain paragraph alone.
        QVERIFY(!html.contains("font-weight: bold\">Plain."));
        // The element's own style wins.
        QVERIFY(html.contains("text-align: right"));

        // Lengths follow the reading text size.
        QCOMPARE(Styling::sized("<p style=\"text-indent: 1.5qe; margin-left: -2qe\">1qe</p>", 20),
                 QString("<p style=\"text-indent: 30px; margin-left: -40px\">1qe</p>"));
        // A chapter that isn't XML is left for the reader to read another way.
        QVERIFY(Styling::apply("<html><body><p>Unclosed</body></html>", {&sheet}).isNull());
    }

    void inkImagesTakeTheThemeColor() {
        // Black lettering on a transparent ground, as Standard Ebooks' title pages are.
        QImage lettering(40, 20, QImage::Format_ARGB32);
        lettering.fill(Qt::transparent);
        for (int x = 5; x < 35; ++x)
            lettering.setPixel(x, 10, qRgba(0, 0, 0, 255));
        QVERIFY(Book::isInk(lettering));

        // A photograph, a colored drawing, and an empty image are left as they are.
        QImage photo(40, 20, QImage::Format_RGB32);
        photo.fill(Qt::black);
        QVERIFY(!Book::isInk(photo));
        QImage drawing = lettering;
        drawing.setPixel(6, 10, qRgba(200, 30, 30, 255));
        QVERIFY(!Book::isInk(drawing));
        QImage blank(40, 20, QImage::Format_ARGB32);
        blank.fill(Qt::transparent);
        QVERIFY(!Book::isInk(blank));

        const QImage light = Book::inked(lettering, QColor("#eeeeee"));
        QCOMPARE(QColor::fromRgba(light.pixel(10, 10)), QColor("#eeeeee"));
        QCOMPARE(qAlpha(light.pixel(10, 5)), 0);

        // The window's color goes on ink URLs only.
        const QString html = "<img src=\"" + Book::inkUrl(3, "a.png") + "\" /><img src=\""
                             + Book::resourceUrl(3, "b.png") + "\" />";
        const QString colored = Book::withInk(html, QColor("#123456"));
        QVERIFY(colored.contains(Book::inkUrl(3, "a.png") + "/123456\""));
        QVERIFY(colored.contains(Book::resourceUrl(3, "b.png") + "\""));
        QCOMPARE(Book::withInk(html, QColor()), html);
    }

    void epubFallsBackToTheNcx() {
        QTemporaryDir dir;
        Book::Error error;
        const auto book = Book::load(writeEpub(dir.path(), true), 1, &error);
        QVERIFY(book);
        QCOMPARE(book->toc.size(), 2);
        QCOMPARE(book->toc[0].title, QString("First"));
        QCOMPARE(book->toc[1].section, 2);
        QCOMPARE(book->toc[1].depth, 1);
    }

    void pdfDrawsItsPages() {
        QTemporaryDir dir;
        Book::Error error;
        const auto book = Book::load(writePdf(dir.path(), 3), 3, &error);
        QVERIFY(book);
        QCOMPARE(book->kind, Book::Kind::Pdf);
        QCOMPARE(book->title, QString("Paper"));
        QCOMPARE(book->sectionCount(), 3);
        QVERIFY(book->paged());
        QVERIFY(qAbs(book->aspects[0] - 148.0 / 210.0) < 0.01);

        const QImage page = book->renderPage(1, QSize(300, 600));
        QCOMPARE(page.width(), 300);
        QCOMPARE(QColor(page.pixel(2, 2)), QColor(Qt::white));
        // Tinted, the paper takes the theme's background.
        const QImage tinted = book->renderPage(1, QSize(300, 600), QColor("#101010"), QColor("#eeeeee"));
        QCOMPARE(QColor(tinted.pixel(2, 2)), QColor("#101010"));

        // Each page says its number, and search finds it on its own page.
        QCOMPARE(book->plainText(2).trimmed(), QString("3"));
        const QList<SearchHit> hits = book->search("2");
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits[0].section, 1);
        QVERIFY(hits[0].rect.y() > 0 && hits[0].rect.bottom() < 0.5);
        QCOMPARE(book->pageText(1, QRectF(0, 0, 1, 0.5)), QString("2"));
        QVERIFY(book->pageText(1, QRectF(0, 0.6, 1, 0.4)).isEmpty());
        QVERIFY(book->pageLinks(1).isEmpty());
    }

    void lockedPdfsNeedTheirPassword() {
        QTemporaryDir dir;
        const QString locked = lockPdf(writePdf(dir.path(), 2), "sesame");
        QVERIFY2(!locked.isEmpty(), "qpdf couldn't lock the PDF");
        Book::Error error;
        QVERIFY(!Book::load(locked, 1, &error));
        QCOMPARE(error, Book::Error::Locked);
        QVERIFY(!Book::load(locked, 1, &error, "wrong"));
        QCOMPARE(error, Book::Error::WrongPassword);
        const auto book = Book::load(locked, 1, &error, "sesame");
        QVERIFY(book);
        QCOMPARE(book->sectionCount(), 2);
        QVERIFY(!book->renderPage(0, QSize(100, 140)).isNull());
        QCOMPARE(book->plainText(1).trimmed(), QString("2"));
    }

    void comicOrdersPagesByNumber() {
        QTemporaryDir dir;
        const QString path = dir.filePath("comic.cbz");
        QVERIFY(writeZip(path, {{"page10.png", png(30, 20)},
                                {"page2.png", png(20, 20)},
                                {"notes.txt", "not a page"},
                                {"__MACOSX/page1.png", "junk"},
                                {"page1.png", png(10, 20)}}));
        Book::Error error;
        const auto book = Book::load(path, 1, &error);
        QVERIFY(book);
        QCOMPARE(book->kind, Book::Kind::Comic);
        QCOMPARE(book->aspects, QList<double>({0.5, 1.0, 1.5}));
        QCOMPARE(book->renderPage(2, QSize(300, 200)).size(), QSize(300, 200));
    }

    void markdownBreaksAtHeadings() {
        QTemporaryDir dir;
        const QString path = writeText(dir.filePath("notes.md"),
                                       "# Field Notes\nIntro.\n\n## One\nText.\n```\n# not a heading\n```\n## Two ##\nMore.\n");
        Book::Error error;
        const auto book = Book::load(path, 1, &error);
        QVERIFY(book);
        QCOMPARE(book->kind, Book::Kind::Markdown);
        QCOMPARE(book->title, QString("Field Notes"));
        QCOMPARE(book->sectionCount(), 3);
        QVERIFY(book->sections[1].contains("# not a heading"));
        QCOMPARE(book->toc.size(), 3);
        QCOMPARE(book->toc[2].title, QString("Two"));
        QCOMPARE(book->toc[2].section, 2);
        QCOMPARE(book->toc[2].depth, 1);
    }

    void markdownTitleStaysWithFirstChapter() {
        QTemporaryDir dir;
        const QString path = writeText(dir.filePath("book.md"),
                                       "# The Lighthouse\n\n## One\nDusk.\n\n## Two\nMorning.\n");
        Book::Error error;
        const auto book = Book::load(path, 1, &error);
        QVERIFY(book);
        QCOMPARE(book->title, QString("The Lighthouse"));
        QCOMPARE(book->sectionCount(), 2);
        QVERIFY(book->sections[0].startsWith("# The Lighthouse"));
        QVERIFY(book->sections[0].contains("Dusk."));
        QCOMPARE(book->toc.size(), 3);
        QCOMPARE(book->toc[0].section, 0);
        QCOMPARE(book->toc[1].section, 0);
        QCOMPARE(book->toc[2].section, 1);
    }

    void textBreaksBetweenParagraphs() {
        QTemporaryDir dir;
        QStringList paragraphs;
        for (int i = 0; i < 3000; ++i)
            paragraphs << QString("Paragraph %1 of a long story.").arg(i);
        const QString text = paragraphs.join("\n\n");
        Book::Error error;
        const auto book = Book::load(writeText(dir.filePath("story.txt"), text.toUtf8()), 1, &error);
        QVERIFY(book);
        QCOMPARE(book->kind, Book::Kind::Text);
        QVERIFY(book->sectionCount() > 3);
        QCOMPARE(book->sections.join("\n\n"), text);
    }

    void unreadableFilesSayWhy() {
        QTemporaryDir dir;
        Book::Error error;
        QVERIFY(!Book::load(dir.filePath("nowhere.pdf"), 1, &error));
        QCOMPARE(error, Book::Error::Missing);
        QVERIFY(!Book::load(writeText(dir.filePath("blob.bin"), QByteArray("\x01\x00\x02", 3)), 1, &error));
        QCOMPARE(error, Book::Error::Unsupported);
        QVERIFY(!Book::load(writeText(dir.filePath("broken.epub"), "PK\x03\x04 not really"), 1, &error));
        QCOMPARE(error, Book::Error::Unreadable);
    }

    void providerDrawsOnlyTheOpenBook() {
        QTemporaryDir dir;
        Book::Error error;
        const auto book = Book::load(writePdf(dir.path(), 2), 5, &error);
        QVERIFY(book);
        QVERIFY(!PageProvider::image(*book, "page/5/1", QSize(200, 300)).isNull());
        QVERIFY(!PageProvider::image(*book, "page/5/0/101010/eeeeee", QSize(200, 300)).isNull());
        QVERIFY(PageProvider::image(*book, "page/4/1", QSize(200, 300)).isNull());
        // Through the engine, a stale request is answered blank, not failed.
        auto open = std::make_shared<OpenBook>();
        PageProvider provider(open);
        QSize size;
        QCOMPARE(provider.requestImage("page/5/1", &size, QSize(200, 300)).size(), QSize(1, 1));
        open->set(book);
        QVERIFY(provider.requestImage("page/5/1", &size, QSize(200, 300)).width() > 1);
        QVERIFY(PageProvider::image(*book, "page/5/9", QSize(200, 300)).isNull());
    }

    void backendRemembersWhereReadingStopped() {
        QTemporaryDir state, files;
        const QString path = writeText(files.filePath("notes.md"), "# A\none\n# B\ntwo\n# C\nthree\n");
        {
            FakePicker picker;
            Backend backend(state.path(), &picker);
            backend.openFile(path);
            QTRY_VERIFY(backend.isOpen());
            QCOMPARE(backend.title(), QString("A"));
            QCOMPARE(backend.sectionCount(), 3);
            QCOMPARE(backend.textFormat(), int(Qt::MarkdownText));
            QVERIFY(backend.text().contains("one"));
            backend.goTo(2, 0.5);
            QVERIFY(backend.text().contains("three"));
            QCOMPARE(backend.tocIndex(), 2);
            QVERIFY(qAbs(backend.progress() - 2.5 / 3) < 1e-9);
        }
        FakePicker picker;
        Backend backend(state.path(), &picker);
        QCOMPARE(backend.library().size(), 1);
        const QVariantMap entry = backend.library().first().toMap();
        QCOMPARE(entry.value("title").toString(), QString("A"));
        QVERIFY(entry.value("exists").toBool());
        QVERIFY(qAbs(entry.value("progress").toDouble() - 2.5 / 3) < 1e-9);
        backend.openRecent(0);
        QTRY_VERIFY(backend.isOpen());
        QCOMPARE(backend.section(), 2);
        QCOMPARE(backend.position(), 0.5);

        backend.close();
        QVERIFY(!backend.isOpen());
        backend.forget(0);
        QVERIFY(backend.library().isEmpty());
    }

    void backendRemembersHowPagesTurn() {
        QTemporaryDir state;
        FakePicker picker;
        {
            Backend backend(state.path(), &picker);
            QCOMPARE(backend.transition(), QString("slide"));
            backend.setTransition("sideways");
            QCOMPARE(backend.transition(), QString("slide"));
            backend.setTransition("cover");
            backend.save();
        }
        Backend again(state.path(), &picker);
        QCOMPARE(again.transition(), QString("cover"));
    }

    void backendCountsProgressByWords() {
        QTemporaryDir state, files;
        FakePicker picker;
        // A title page with no reading in it, a long chapter and a short one.
        QString markdown = "# Title\n\n## Long\n";
        for (int i = 0; i < 100; ++i)
            markdown += "one two three four five six seven eight nine ten\n\n";
        markdown += "## Short\nOne two three four five six seven eight nine ten.\n";
        {
            Backend backend(state.path(), &picker);
            backend.openFile(writeText(files.filePath("words.md"), markdown.toUtf8()));
            QTRY_VERIFY(backend.isOpen());
            QCOMPARE(backend.sectionCount(), 2);
            // A thousand words and more at 250 a minute.
            QCOMPARE(backend.wordsPerMinute(), 250.0);
            QCOMPARE(backend.minutesLeftInChapter(), 5);
            QCOMPARE(backend.minutesLeftInBook(), 5);
            // Halfway through the long chapter is nearly halfway through the
            // book: the short chapter weighs little, not half.
            backend.setPosition(0.5);
            QVERIFY(backend.progress() > 0.48 && backend.progress() < 0.5);
            QCOMPARE(backend.minutesLeftInChapter(), 3);
            // The reader's pace is learned a little at a time, and screens too
            // short or too long to say anything are left out.
            backend.noteReading(1, 0.2);
            backend.noteReading(1000, 0.2);
            QCOMPARE(backend.wordsPerMinute(), 250.0);
            backend.noteReading(30, 0.2);    // about 200 words in 30 seconds: 400 a minute
            QVERIFY(backend.wordsPerMinute() > 260 && backend.wordsPerMinute() < 270);
            backend.setProgressShows("book");
            backend.setProgressShows("sideways");
            QCOMPARE(backend.progressShows(), QString("book"));
            backend.save();
        }
        // The pace, the choice, and the progress the library shows are kept.
        Backend again(state.path(), &picker);
        QVERIFY(again.wordsPerMinute() > 260);
        QCOMPARE(again.progressShows(), QString("book"));
        const QVariantMap entry = again.library().value(0).toMap();
        QVERIFY(entry.value("progress").toDouble() > 0.48 && entry.value("progress").toDouble() < 0.5);
    }

    void backendNamesThePartAndChapter() {
        QTemporaryDir state, files;
        FakePicker picker;
        Backend backend(state.path(), &picker);
        backend.openFile(writeText(files.filePath("parts.md"),
                                   "# Novel\nIntro.\n## Part One\nText.\n## Part Two\nMore.\n"));
        QTRY_VERIFY(backend.isOpen());
        // The book's own title is never repeated as the chapter's.
        QCOMPARE(backend.chapterLabel(), QString());
        backend.goTo(2);
        QCOMPARE(backend.chapterLabel(), QString("Part Two"));
    }

    void backendKeepsBookmarks() {
        QTemporaryDir state, files;
        FakePicker picker;
        const QString path = writeText(files.filePath("marks.md"), "# Book\nOne.\n## Two\nTwo.\n## Three\nThree.\n");
        {
            Backend backend(state.path(), &picker);
            backend.openFile(path);
            QTRY_VERIFY(backend.isOpen());
            backend.goTo(1);
            QVERIFY(backend.toggleBookmark(0, 1, "Two.  "));
            backend.goTo(0);
            QVERIFY(backend.toggleBookmark(0.2, 0.5, "One."));
            // In reading order, with the first words there and the chapter.
            QCOMPARE(backend.bookmarks().size(), 2);
            QCOMPARE(backend.bookmarks()[0].toMap().value("text").toString(), QString("One."));
            QCOMPARE(backend.bookmarks()[1].toMap().value("chapter").toString(), QString("Two"));
            // B on a marked page takes the mark away.
            QVERIFY(!backend.toggleBookmark(0.1, 0.4, ""));
            QCOMPARE(backend.bookmarks().size(), 1);
            // Going to one leaves a place to come back to.
            backend.goToBookmark(0);
            QCOMPARE(backend.section(), 1);
            QVERIFY(backend.goBack());
            QCOMPARE(backend.section(), 0);
            // Another book and back: they're still there.
            backend.close();
            QVERIFY(backend.bookmarks().isEmpty());
            backend.openFile(path);
            QTRY_VERIFY(backend.isOpen());
            QCOMPARE(backend.bookmarks().size(), 1);
            backend.save();
        }
        Backend again(state.path(), &picker);
        again.openFile(path);
        QTRY_VERIFY(again.isOpen());
        QCOMPARE(again.bookmarks().size(), 1);
        again.removeBookmark(0);
        QVERIFY(again.bookmarks().isEmpty());
    }

    void backendRemembersHowTextIsSet() {
        QTemporaryDir state;
        FakePicker picker;
        {
            Backend backend(state.path(), &picker);
            QCOMPARE(backend.typeface(), QString("serif"));
            QCOMPARE(backend.lineSpacing(), QString("normal"));
            QCOMPARE(backend.columnWidth(), QString("medium"));
            QVERIFY(!backend.justify());
            backend.setTypeface("comic");
            QCOMPARE(backend.typeface(), QString("serif"));
            backend.setTypeface("sans");
            backend.setLineSpacing("relaxed");
            backend.setColumnWidth("wide");
            backend.setJustify(true);
            backend.save();
        }
        Backend again(state.path(), &picker);
        QCOMPARE(again.typeface(), QString("sans"));
        QCOMPARE(again.lineSpacing(), QString("relaxed"));
        QCOMPARE(again.columnWidth(), QString("wide"));
        QVERIFY(again.justify());
    }

    void backendAsksForAPassword() {
        QTemporaryDir state, files;
        FakePicker picker;
        Backend backend(state.path(), &picker);
        QSignalSpy asked(&backend, &Backend::passwordChanged);
        backend.openFile(lockPdf(writePdf(files.path(), 2), "sesame"));
        QTRY_COMPARE(backend.passwordFor(), QString("locked.pdf"));
        QVERIFY(!backend.passwordWrong());
        QVERIFY(backend.error().isEmpty());
        backend.unlock("wrong");
        QTRY_VERIFY(backend.passwordWrong());
        QVERIFY(!backend.isOpen());
        backend.unlock("sesame");
        QTRY_VERIFY(backend.isOpen());
        QVERIFY(backend.passwordFor().isEmpty());
        QCOMPARE(backend.sectionCount(), 2);
    }

    void backendSearchesAndFollowsLinks() {
        QTemporaryDir state, files;
        FakePicker picker;
        Backend backend(state.path(), &picker);
        backend.openFile(writeEpub(files.path()));
        QTRY_VERIFY(backend.isOpen());
        QCOMPARE(backend.section(), 0);

        backend.find("  the   END ");
        QVERIFY(backend.searching());
        QTRY_VERIFY(!backend.searching());
        QCOMPARE(backend.query(), QString("the END"));
        QCOMPARE(backend.hitCount(), 1);
        QCOMPARE(backend.hitIndex(), 0);
        QCOMPARE(backend.section(), 2);
        QCOMPARE(backend.hitOccurrence(), 0);
        backend.clearSearch();
        QCOMPARE(backend.hitCount(), 0);

        backend.goTo(1);
        QCOMPARE(backend.target(), -1);
        QVERIFY(backend.followLink("omaread:2#later"));
        QCOMPARE(backend.section(), 2);
        QCOMPARE(backend.target(), int(QString("Two\nBefore.\n").size()));
        QVERIFY(!backend.followLink("https://example.com/"));
        backend.goToEntry(0);
        QCOMPARE(backend.section(), 1);
        QCOMPARE(backend.target(), -1);

        // A PDF's matches are areas on its pages, two at a time in a spread.
        backend.openFile(writePdf(files.path(), 4));
        QTRY_VERIFY(backend.paged());
        backend.setSpread(true);
        backend.find("3");
        QTRY_COMPARE(backend.hitCount(), 1);
        QCOMPARE(backend.section(), 2);
        QCOMPARE(backend.pageHits().size(), 1);
        QVERIFY(backend.pageHits().first().toMap().value("current").toBool());
        QCOMPARE(backend.progress(), 1.0);
        QVERIFY(backend.copyPageText(2, 0, 0, 1, 0.5));
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("3"));
        QVERIFY(!backend.copyPageText(2, 0, 0.6, 1, 0.4));
    }

    void backendReportsWhatWentWrong() {
        QTemporaryDir state, files;
        FakePicker picker;
        Backend backend(state.path(), &picker);
        backend.openFile(files.filePath("gone.epub"));
        QTRY_VERIFY(!backend.loading());
        QCOMPARE(backend.error(), QString("gone.epub isn't there any more."));
        QVERIFY(!backend.isOpen());
        backend.openFile(writeText(files.filePath("blob.bin"), QByteArray("\x01\x00\x02", 3)));
        QTRY_VERIFY(backend.error().contains("can't read blob.bin"));
        backend.dismissError();
        QVERIFY(backend.error().isEmpty());
        QVERIFY(backend.library().isEmpty());
        emit picker.failed("The portal is away.");
        QCOMPARE(backend.error(), QString("The portal is away."));
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeFollowsTheDesktopTextSize() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        Theme theme(dir.path());
        QCOMPARE(theme.textSize(), 12);

        // The theme's own size, then the user's choice over it, live.
        QVERIFY(write(dir.path() + "/theme/shell.toml", "[font]\nbase-size = 13\n"));
        QTRY_COMPARE(theme.textSize(), 13);
        QVERIFY(write(dir.path() + "/shell.toml", "[bar]\nbase-size = 30\n\n[font]\nfamily = \"x\"\nbase-size = 16 # px\n"));
        QTRY_COMPARE(theme.textSize(), 16);
        QCOMPARE(Theme::readBaseSize(dir.path() + "/missing.toml"), 0);
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void everyCatalogIsCompiledIn() {
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files))
            QVERIFY2(QFile::exists(":/i18n/" + file.fileName()), qPrintable(file.fileName() + " is not in resources.qrc"));
    }

    void commandsReadBooksWithoutADisplay() {
        QTemporaryDir dir;
        const QString epub = writeEpub(dir.path());

        Run run = omaread({"--version"});
        QCOMPARE(run.code, 0);
        QVERIFY(run.out.startsWith("omaread "));
        run = omaread({});
        QCOMPARE(run.code, 0);
        QVERIFY(run.out.contains("info <file>"));

        run = omaread({"info", epub, "--json"});
        QCOMPARE(run.code, 0);
        const QJsonObject info = QJsonDocument::fromJson(run.out).object();
        QCOMPARE(info.value("title").toString(), QString("A Small & Quiet Book"));
        QCOMPARE(info.value("format").toString(), QString("epub"));
        QCOMPARE(info.value("sections").toInt(), 3);
        QCOMPARE(info.value("contents").toArray().at(1).toObject().value("section").toInt(), 3);

        run = omaread({"text", epub, "--section", "3"});
        QCOMPARE(run.code, 0);
        QCOMPARE(QString::fromUtf8(run.out).trimmed(), QString("Two\nBefore.\nThe end."));

        run = omaread({"search", epub, "the", "end", "--json"});
        QCOMPARE(run.code, 0);
        QCOMPARE(QJsonDocument::fromJson(run.out).object().value("matches").toInt(), 1);

        // Errors go to stderr with a failing exit code.
        run = omaread({"text", epub, "--section", "9"});
        QCOMPARE(run.code, 1);
        QVERIFY(run.err.contains("from 1 to 3"));
        run = omaread({"info", dir.filePath("gone.pdf")});
        QCOMPARE(run.code, 1);
        QVERIFY(run.err.contains("Can't find"));
        const QString locked = lockPdf(writePdf(dir.path(), 1), "sesame");
        QCOMPARE(omaread({"info", locked}).code, 1);
        QCOMPARE(omaread({"text", locked, "--password", "sesame"}).out.trimmed(), QByteArray("1"));

        run = omaread({"skill"});
        QCOMPARE(run.code, 0);
        QVERIFY(run.out.startsWith("---\nname: omaread"));
    }

    void windowSetsTheTextFromTheSettings() {
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(window->property("settled").toBool());
        backend.setTransition("none");

        backend.openFile(writeText(files.filePath("story.md"),
                                   "# Story\n\nA first paragraph, long enough to run over more than one line of the column.\n"));
        QTRY_VERIFY(backend.isOpen());
        auto *prose = item(window->contentItem(), "prose");
        QVERIFY(prose);
        auto *document = prose->property("textDocument").value<QQuickTextDocument *>()->textDocument();
        const auto paragraph = [&] { return document->lastBlock().blockFormat(); };
        QTRY_COMPARE(paragraph().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignLeading);
        const double lineHeight = paragraph().lineHeight();
        const double width = prose->width();

        // S opens the settings, and the key right after it already goes to them.
        QTest::keyClick(window, Qt::Key_S);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.typeface(), QString("sans"));
        QCOMPARE(backend.section(), 0);
        // Line spacing, the column, and justified lines, each two rows on.
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.lineSpacing(), QString("relaxed"));
        QTRY_VERIFY(paragraph().lineHeight() > lineHeight);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.columnWidth(), QString("wide"));
        QTRY_VERIFY(prose->width() > width);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Right);
        QVERIFY(backend.justify());
        QTRY_COMPARE(paragraph().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignJustify);
        // The heading isn't justified, and S closes the settings again.
        QCOMPARE(document->firstBlock().blockFormat().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignLeft);
        QObject *panel = window->findChild<QObject *>("settings");
        QVERIFY(panel && panel->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_S);
        QTRY_VERIFY(!panel->property("visible").toBool());
        QTRY_VERIFY(!window->property("busy").toBool());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void epubKeepsItsNotes() {
        QTemporaryDir dir;
        Book::Error error;
        const auto book = Book::load(writeNotedEpub(dir.path()), 1, &error);
        QVERIFY(book);
        QCOMPARE(book->sectionCount(), 2);
        // The note, styled like the text, without its link back.
        const QString note = book->notes.value("1#note-1");
        QVERIFY(note.contains("The emancipation of 1861 is <i>meant</i>."));
        QVERIFY(!note.contains("back"));
        // The reference to it is a link to that place, its number raised.
        QVERIFY(book->sections[0].contains(
            "<a href=\"omaread:1#note-1\" id=\"noteref-1\" style=\"text-decoration: none; vertical-align: super\">1</a>"));
    }

    void windowJumpsToAChaptersStart() {
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(window->property("settled").toBool());
        backend.setTransition("none");

        QString markdown;
        for (const QString chapter : {"One", "Two"}) {
            markdown += "## " + chapter + "\n";
            for (int i = 0; i < 300; ++i)
                markdown += QString("Line %1 of chapter %2.\n\n").arg(i).arg(chapter);
        }
        backend.openFile(writeText(files.filePath("two.md"), markdown.toUtf8()));
        QTRY_VERIFY(backend.isOpen());
        auto *view = item(window->contentItem(), "view");
        // Halfway down the first chapter, the next one opens at its start.
        backend.setPosition(0.5);
        QMetaObject::invokeMethod(window, "restore");
        QTRY_VERIFY(view->property("contentY").toDouble() > 1000);
        QTest::keyClick(window, Qt::Key_BracketRight);
        QCOMPARE(backend.section(), 1);
        QTest::qWait(300);
        QCOMPARE(backend.position(), 0.0);
        QCOMPARE(view->property("contentY").toDouble(), 0.0);

        // A jump leaves a place to come back to: halfway down the first chapter,
        // End goes to the last, Backspace returns, and Shift+Backspace goes on.
        QVERIFY(!backend.canGoBack());
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(backend.section(), 0);
        backend.setPosition(0.5);
        QMetaObject::invokeMethod(window, "restore");
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(backend.section(), 1);
        QVERIFY(backend.canGoBack());
        QTest::keyClick(window, Qt::Key_Backspace);
        QCOMPARE(backend.section(), 0);
        QCOMPARE(backend.position(), 0.5);
        QTRY_VERIFY(view->property("contentY").toDouble() > 1000);
        QVERIFY(backend.canGoForward());
        QTest::keyClick(window, Qt::Key_Backspace, Qt::ShiftModifier);
        QCOMPARE(backend.section(), 1);
        // The contents' jumps can be gone back from too.
        backend.goToEntry(0);
        QCOMPARE(backend.section(), 0);
        QVERIFY(backend.goBack());
        QCOMPARE(backend.section(), 1);
        // B marks the page, its corner shows it, and the contents' second tab
        // lists it: Enter goes there.
        QTest::keyClick(window, Qt::Key_B);
        QCOMPARE(backend.bookmarks().size(), 1);
        QTRY_VERIFY(window->property("bookmarkHere").toBool());
        QTest::keyClick(window, Qt::Key_Home);
        QVERIFY(!window->property("bookmarkHere").toBool());
        QTest::keyClick(window, Qt::Key_T);
        QObject *contents = window->findChild<QObject *>("contents");
        QTRY_VERIFY(contents->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Tab);
        QVERIFY(contents->property("marks").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!contents->property("visible").toBool());
        QCOMPARE(backend.section(), 1);
        QTRY_VERIFY(window->property("bookmarkHere").toBool());
    }

    void windowCountsPagesAsSpaceTurnsThem() {
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(window->property("settled").toBool());
        backend.setTransition("none");

        // Paragraphs of different lengths, so pages break inside them.
        QString markdown = "## One\n";
        for (int i = 0; i < 120; ++i)
            markdown += QString("Paragraph %1. ").arg(i) + QString("Words run on here. ").repeated(1 + i % 7) + "\n\n";
        markdown += "## Two\nThe end.\n";
        backend.openFile(writeText(files.filePath("pages.md"), markdown.toUtf8()));
        QTRY_VERIFY(backend.isOpen());
        QTRY_VERIFY(backend.pageCount() > 5);
        auto *view = item(window->contentItem(), "view");
        // Every Space is one page on, and the chapter's last screen its last page.
        int page = backend.pageAt(0, view->property("contentY").toDouble());
        QCOMPARE(page, 1);
        while (backend.section() == 0) {
            const double before = view->property("contentY").toDouble();
            QTest::keyClick(window, Qt::Key_Space);
            if (backend.section() != 0)
                break;
            QTRY_VERIFY(view->property("contentY").toDouble() > before);
            QCOMPARE(backend.pageAt(0, view->property("contentY").toDouble()), ++page);
        }
        QCOMPARE(page, backend.pageCount() - 1);
        QCOMPARE(backend.pageAt(1, 0), backend.pageCount());
    }

    void windowShowsANoteInPlace() {
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QTRY_VERIFY(window->property("settled").toBool());
        backend.setTransition("none");

        backend.openFile(writeNotedEpub(files.path()));
        QTRY_VERIFY(backend.isOpen());
        QVERIFY(backend.note("omaread:0").isEmpty());
        QVERIFY(backend.note("https://example.com/").isEmpty());

        // Choosing the note's number shows the note and stays on the page.
        auto *prose = item(window->contentItem(), "prose");
        QVERIFY(prose);
        QMetaObject::invokeMethod(prose, "linkActivated", Q_ARG(QString, "omaread:1#note-1"));
        QObject *note = window->findChild<QObject *>("note");
        QVERIFY(note);
        QTRY_VERIFY(note->property("visible").toBool());
        QVERIFY(note->property("html").toString().contains("emancipation"));
        QCOMPARE(backend.section(), 0);
        if (!qEnvironmentVariableIsEmpty("OMAREAD_GRAB")) {
            QTest::qWait(400);
            window->grabWindow().save(qEnvironmentVariable("OMAREAD_GRAB"));
        }
        // Escape closes it, still on the page.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!note->property("visible").toBool());
        QCOMPARE(backend.section(), 0);
        QVERIFY(backend.isOpen());
        // Enter goes to the note itself.
        QMetaObject::invokeMethod(prose, "linkActivated", Q_ARG(QString, "omaread:1#note-1"));
        QTRY_VERIFY(note->property("visible").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!note->property("visible").toBool());
        QCOMPARE(backend.section(), 1);

        // From the keyboard: Tab chooses the note's number, Enter opens it.
        QVERIFY(backend.goBack());
        QCOMPARE(backend.section(), 0);
        QTest::keyClick(window, Qt::Key_Tab);
        QCOMPARE(prose->property("selectedText").toString(), QString("1"));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(note->property("visible").toBool());
        QCOMPARE(backend.section(), 0);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!note->property("visible").toBool());
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void windowShowsAChaptersImagesAfterAJump() {
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QTRY_VERIFY(window->property("settled").toBool());
        backend.setTransition("none");

        // The second chapter's picture is first asked for after the jump to it.
        backend.openFile(writeEpub(files.path()));
        QTRY_VERIFY(backend.isOpen());
        backend.goTo(1);
        auto *prose = item(window->contentItem(), "prose");
        QVERIFY(prose);
        const QUrl wide(Book::resourceUrl(backend.bookId(), "OEBPS/images/wide.png"));
        QTRY_VERIFY(prose->property("text").toString().contains(wide.toString()));
        auto *document = prose->property("textDocument").value<QQuickTextDocument *>();
        QVERIFY(document);
        const QVariant image = document->textDocument()->resource(QTextDocument::ImageResource, wide);
        QVERIFY(image.isValid());
        QCOMPARE(image.value<QImage>().width(), 2000);
    }

    void windowReadsFromTheKeyboard() {
        // A temp language, theme and library, so the test never depends on the desktop's.
        QTemporaryDir dir, files;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        FakePicker picker;
        Backend backend(dir.path() + "/state", &picker);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.addImageProvider("omaread", backend.createPageProvider());
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        // The contents appear, and take keys, once the window's size is in.
        QTRY_VERIFY(window->property("settled").toBool());
        // Pages turn at once here; the turns themselves are tested below.
        backend.setTransition("none");

        // An empty library says how to start, and O asks for a file.
        QVERIFY(item(window->contentItem(), "empty")->isVisible());
        QTest::keyClick(window, Qt::Key_O);
        QCOMPARE(picker.asked, 1);

        QString markdown = "# Long\n";
        for (int i = 0; i < 400; ++i)
            markdown += QString("Line %1 of the first chapter.\n\n").arg(i);
        markdown += "## Second\nShort.\n## Third\nShorter.\n";
        backend.openFile(writeText(files.filePath("long.md"), markdown.toUtf8()));
        QTRY_VERIFY(backend.isOpen());
        auto *where = item(window->contentItem(), "where");
        QVERIFY(where);
        // The footer tells the time left in the chapter at 250 words a minute,
        // and P steps through the rest, ending back where it started.
        QCOMPARE(where->property("text").toString(), QString("10 min left in chapter"));
        // The chapter isn't named when it only repeats the book's title.
        QCOMPARE(item(window->contentItem(), "chapter")->property("text").toString(), QString());
        QTRY_VERIFY(backend.pageCount() > 0);
        const QStringList shown = {"10 min left in book", "Page 1 of " + QString::number(backend.pageCount()), "0%", "",
                                   "10 min left in chapter"};
        for (const QString &text : shown) {
            QTest::keyClick(window, Qt::Key_P);
            QCOMPARE(where->property("text").toString(), text);
        }

        // Space pages down a screen within the chapter, then on to the next.
        auto *view = item(window->contentItem(), "view");
        QTest::keyClick(window, Qt::Key_Space);
        QTRY_VERIFY(view->property("contentY").toDouble() > 100);
        QVERIFY(backend.position() > 0);
        QCOMPARE(backend.section(), 0);
        QTest::keyClick(window, Qt::Key_End);
        QCOMPARE(backend.section(), 2);
        QCOMPARE(item(window->contentItem(), "chapter")->property("text").toString(), QString("Third"));
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QCOMPARE(backend.section(), 1);
        QTest::keyClick(window, Qt::Key_Home);
        QCOMPARE(backend.section(), 0);

        // The contents choose a chapter with Down and Enter.
        auto *contents = window->findChild<QObject *>("contents");
        QTest::keyClick(window, Qt::Key_T);
        QTRY_VERIFY(contents->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!contents->property("visible").toBool());
        QCOMPARE(backend.section(), 2);

        // The text size grows from the desktop's, and 0 puts it back.
        auto *prose = item(window->contentItem(), "prose");
        QCOMPARE(prose->property("font").value<QFont>().pixelSize(), 18);
        QTest::keyClick(window, Qt::Key_Plus);
        QCOMPARE(backend.textScale(), 1.6);
        QTRY_COMPARE(prose->property("font").value<QFont>().pixelSize(), 19);
        QTest::keyClick(window, Qt::Key_0);
        QCOMPARE(backend.textScale(), Backend::defaultTextScale);

        // A turned page animates, and lands where it should. A key pressed
        // during a turn ends it and turns again.
        backend.setTransition("slide");
        backend.goTo(0);
        QTest::keyClick(window, Qt::Key_BracketRight);
        QVERIFY(window->property("turning").toBool());
        QTRY_COMPARE(backend.section(), 1);
        QTRY_COMPARE(window->property("turnKind").toString(), QString("slide"));
        QTest::keyClick(window, Qt::Key_BracketRight);
        QTRY_COMPARE(backend.section(), 2);
        QTRY_VERIFY(!window->property("turning").toBool());
        // A picks the next kind of turn, and says which.
        QTest::keyClick(window, Qt::Key_A);
        QCOMPARE(backend.transition(), QString("flip"));
        QCOMPARE(window->property("toast").toString(), QString("Page turn: Flip"));
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QTRY_COMPARE(backend.section(), 1);
        QTRY_VERIFY(!window->property("turning").toBool());
        for (int i = 0; i < 3; ++i)
            QTest::keyClick(window, Qt::Key_A);
        QCOMPARE(backend.transition(), QString("none"));
        QTest::keyClick(window, Qt::Key_BracketLeft);
        QCOMPARE(backend.section(), 0);
        QVERIFY(!window->property("turning").toBool());

        // G goes to a section by its number.
        auto *goDialog = window->findChild<QObject *>("goDialog");
        QTest::keyClick(window, Qt::Key_G);
        QTRY_VERIFY(goDialog->property("opened").toBool());
        typeInto(window, "2");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!goDialog->property("visible").toBool());
        QCOMPARE(backend.section(), 1);

        // / searches; the match is selected, and Ctrl+C copies it.
        QTest::keyClick(window, Qt::Key_Slash);
        auto *findField = item(window->contentItem(), "findField");
        QTRY_VERIFY(findField->hasActiveFocus());
        typeInto(window, "shorter");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(backend.hitCount(), 1);
        QCOMPARE(backend.section(), 2);
        QTRY_COMPARE(prose->property("selectedText").toString(), QString("Shorter"));
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!findField->hasActiveFocus());
        QCOMPARE(backend.hitCount(), 0);
        prose->setProperty("cursorPosition", 0);
        QMetaObject::invokeMethod(prose, "select", Q_ARG(int, 0), Q_ARG(int, 5));
        QTest::keySequence(window, QKeySequence::Copy);
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("Third"));

        // A PDF turns its pages with the arrows.
        backend.openFile(writePdf(files.path(), 3));
        QTRY_VERIFY(backend.paged());
        QTRY_COMPARE(item(window->contentItem(), "pageImage")->property("status").toInt(), 1);
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.section(), 1);
        // A PDF has no words to time, so it shows its pages.
        QCOMPARE(where->property("text").toString(), QString("Page 2 of 3"));
        QTest::keyClick(window, Qt::Key_I);
        QVERIFY(backend.tintPages());
        QTest::keyClick(window, Qt::Key_I);
        // D shows two pages side by side, and turns two at a time.
        QTest::keyClick(window, Qt::Key_D);
        QVERIFY(backend.spread());
        QTRY_COMPARE(item(window->contentItem(), "spread")->childItems().size(), 3);
        QCOMPARE(where->property("text").toString(), QString("Pages 2–3 of 3"));
        QTest::keyClick(window, Qt::Key_Left);
        QCOMPARE(backend.section(), 0);
        QTest::keyClick(window, Qt::Key_D);
        QVERIFY(!backend.spread());
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(backend.section(), 1);

        // A locked PDF asks for its password in the window.
        auto *passwordDialog = window->findChild<QObject *>("passwordDialog");
        backend.openFile(lockPdf(writePdf(files.path(), 2), "sesame"));
        QTRY_VERIFY(passwordDialog->property("opened").toBool());
        typeInto(window, "sesame");
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_COMPARE(backend.sectionCount(), 2);
        QCOMPARE(backend.title(), QString("Paper"));
        QTRY_VERIFY(!passwordDialog->property("visible").toBool());

        // Escape goes back to the library, where both books wait.
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!backend.isOpen());
        auto *shelf = item(window->contentItem(), "shelf");
        QTRY_COMPARE(shelf->property("count").toInt(), 3);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(backend.isOpen());
        QCOMPARE(backend.kind(), QString("markdown"));
        QCOMPARE(backend.section(), 2);

        auto *help = window->findChild<QObject *>("help");
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->property("opened").toBool());
        // A Material popup animates out: `opened` drops at once, `visible` after.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(OmareadTests)
#include "omaread_tests.moc"
