#include "book.h"
#include "stylesheet.h"

#include <QBuffer>
#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTextDocument>
#include <QThread>
#include <QUrl>
#include <QXmlStreamReader>

#include <poppler-qt6.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <functional>

namespace {

// Large enough for any real book or comic, small enough that a hostile
// archive can't take all the memory.
constexpr qint64 maxEntrySize = 512ll * 1024 * 1024;
constexpr qint64 maxArchiveSize = 2048ll * 1024 * 1024;
// Plain text is cut into sections about this long, so the window lays out a
// chapter's worth of text at a time rather than a whole novel.
constexpr int textSectionLength = 20000;

struct Entry {
    QString name;
    QByteArray data;
};

// Every regular file in a ZIP, RAR, 7z or tar archive, in archive order.
bool readArchive(const QString &path, QList<Entry> *entries, Book::Error *error) {
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK) {
        archive_read_free(a);
        *error = Book::Error::Unreadable;
        return false;
    }
    qint64 total = 0;
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (archive_entry_filetype(entry) != AE_IFREG)
            continue;
        const la_int64_t size = archive_entry_size(entry);
        if (size > maxEntrySize || (total += size) > maxArchiveSize) {
            status = ARCHIVE_FATAL;
            break;
        }
        const char *utf8 = archive_entry_pathname_utf8(entry);
        Entry file{utf8 ? QString::fromUtf8(utf8) : QString::fromLocal8Bit(archive_entry_pathname(entry)), {}};
        if (size > 0)
            file.data.reserve(size);
        char buffer[1 << 16];
        la_ssize_t read = 0;
        while ((read = archive_read_data(a, buffer, sizeof buffer)) > 0) {
            if (file.data.size() + read > maxEntrySize)
                break;
            file.data.append(buffer, read);
        }
        if (read < 0) {
            status = ARCHIVE_FATAL;
            break;
        }
        entries->append(std::move(file));
    }
    archive_read_free(a);
    if (status != ARCHIVE_EOF) {
        *error = Book::Error::Unreadable;
        return false;
    }
    return true;
}

// HTML's named entities aren't XML's, and an EPUB's XHTML uses them anyway.
// QXmlStreamReader stops at the first one it doesn't know, so name them by
// number first. An unknown one becomes a space rather than ending the parse.
QByteArray xmlEntities(QByteArray xml) {
    static const QHash<QByteArray, QByteArray> named = {
        {"nbsp", "&#160;"},   {"mdash", "&#8212;"}, {"ndash", "&#8211;"}, {"hellip", "&#8230;"},
        {"lsquo", "&#8216;"}, {"rsquo", "&#8217;"}, {"ldquo", "&#8220;"}, {"rdquo", "&#8221;"},
        {"laquo", "&#171;"},  {"raquo", "&#187;"},  {"copy", "&#169;"},   {"reg", "&#174;"},
        {"trade", "&#8482;"}, {"shy", "&#173;"},    {"middot", "&#183;"}, {"bull", "&#8226;"},
        {"eacute", "&#233;"}, {"egrave", "&#232;"}, {"agrave", "&#224;"}, {"ccedil", "&#231;"},
        {"ouml", "&#246;"},   {"uuml", "&#252;"},   {"auml", "&#228;"},   {"szlig", "&#223;"},
        {"aring", "&#229;"},  {"oslash", "&#248;"}, {"aelig", "&#230;"},  {"thinsp", "&#8201;"},
        {"ensp", "&#8194;"},  {"emsp", "&#8195;"},  {"zwnj", "&#8204;"},  {"zwj", "&#8205;"},
    };
    static const QByteArrayList builtin = {"amp", "lt", "gt", "quot", "apos"};
    QByteArray out;
    out.reserve(xml.size());
    qsizetype i = 0;
    while (i < xml.size()) {
        const qsizetype amp = xml.indexOf('&', i);
        if (amp < 0) {
            out.append(xml.mid(i));
            break;
        }
        out.append(xml.mid(i, amp - i));
        qsizetype end = amp + 1;
        while (end < xml.size() && end - amp < 12 && std::isalnum(static_cast<unsigned char>(xml[end])))
            ++end;
        const QByteArray name = xml.mid(amp + 1, end - amp - 1);
        if (end < xml.size() && xml[end] == ';' && !name.isEmpty() && !builtin.contains(name)) {
            out.append(named.value(name, " "));
            i = end + 1;
        } else {
            out.append('&');
            i = amp + 1;
        }
    }
    return out;
}

// A path inside the archive, from an href relative to the file at from.
QString resolve(const QString &from, const QString &href) {
    const QUrl url(href);
    if (!url.scheme().isEmpty())
        return {};
    const QString path = url.path(QUrl::FullyDecoded);
    if (path.isEmpty())
        return from;
    const QString folder = QFileInfo(from).path();
    QString resolved = QDir::cleanPath(folder == "." || path.startsWith('/') ? path : folder + '/' + path);
    while (resolved.startsWith('/'))
        resolved.remove(0, 1);
    return resolved;
}

QString attribute(const QString &tag, const QString &name) {
    const QRegularExpression pattern("\\b" + QRegularExpression::escape(name)
                                         + "\\s*=\\s*(\"([^\"]*)\"|'([^']*)')",
                                     QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(tag);
    if (!match.hasMatch())
        return {};
    return match.captured(2).isNull() ? match.captured(3) : match.captured(2);
}

QString decodeHtml(const QString &text) {
    return QString(text).replace("&quot;", "\"").replace("&apos;", "'").replace("&lt;", "<").replace("&gt;", ">").replace(
        "&amp;", "&");
}

QString escapeAttribute(const QString &text) {
    return text.toHtmlEscaped();
}

// The parts of an inline style that shape text rather than paint it. Colors,
// fonts and sizes are the reader's to choose, so the theme and the text size
// apply to every book alike.
QString keptStyle(const QString &style) {
    static const QStringList kept = {"font-style", "font-weight", "text-align", "text-decoration", "font-variant",
                                     "vertical-align"};
    QStringList out;
    for (const QString &declaration : style.split(';', Qt::SkipEmptyParts)) {
        const QString property = declaration.section(':', 0, 0).trimmed().toLower();
        if (kept.contains(property))
            out << declaration.trimmed();
    }
    return out.join("; ");
}

QSize imageSize(const QByteArray &data) {
    QByteArray copy = data;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return reader.size();
}

// An image no bigger than 128 px, enough to tell what kind of image it is.
QImage smallImage(const QByteArray &data) {
    QByteArray copy = data;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 128 || size.height() > 128))
        reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
    return reader.read();
}

bool isImageName(const QString &name) {
    static const QStringList extensions = {"jpg", "jpeg", "png", "webp", "gif", "bmp", "avif", "jxl", "tif", "tiff"};
    return extensions.contains(QFileInfo(name).suffix().toLower());
}

QString decodeText(const QByteArray &bytes) {
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8.decode(bytes);
    // Not UTF-8: older text files are mostly Latin-1, which can't fail.
    return utf8.hasError() ? QString::fromLatin1(bytes) : text;
}

} // namespace

Book::~Book() = default;

std::shared_ptr<Book> Book::load(const QString &path, int id, Error *error, const QString &password) {
    *error = Error::None;
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        *error = Error::Missing;
        return nullptr;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = Error::Unreadable;
        return nullptr;
    }
    const QByteArray head = file.read(4096);
    file.close();

    std::shared_ptr<Book> book(new Book);
    book->id = id;
    book->path = info.absoluteFilePath();
    book->title = info.completeBaseName();

    const QString suffix = info.suffix().toLower();
    const bool zip = head.startsWith("PK\x03\x04");
    if (head.startsWith("%PDF") || suffix == "pdf")
        book->kind = Kind::Pdf;
    else if (suffix == "epub" || (zip && head.mid(30, 28) == "mimetypeapplication/epub+zip"))
        book->kind = Kind::Epub;
    else if (QStringList{"cbz", "cbr", "cb7", "cbt"}.contains(suffix) || zip)
        book->kind = Kind::Comic;
    else if (suffix == "md" || suffix == "markdown")
        book->kind = Kind::Markdown;
    else if (!head.contains('\0'))
        book->kind = Kind::Text;
    else {
        *error = Error::Unsupported;
        return nullptr;
    }

    bool loaded = false;
    switch (book->kind) {
    case Kind::Pdf: loaded = book->loadPdf(error, password); break;
    case Kind::Epub: loaded = book->loadEpub(error); break;
    case Kind::Comic: loaded = book->loadComic(error); break;
    case Kind::Markdown: loaded = book->loadMarkdown(error); break;
    case Kind::Text: loaded = book->loadText(error); break;
    }
    if (!loaded)
        return nullptr;
    if (book->sectionCount() == 0) {
        *error = Error::Empty;
        return nullptr;
    }
    if (!book->paged()) {
        static const QRegularExpression tag("<[^>]*>");
        static const QRegularExpression word("[\\w\\p{L}][\\w\\p{L}'’-]*");
        for (const QString &section : std::as_const(book->sections)) {
            const QString text = book->kind == Kind::Epub ? QString(section).replace(tag, " ") : section;
            int count = 0;
            for (auto it = word.globalMatch(text); it.hasNext(); it.next())
                ++count;
            book->words << count;
        }
        while (book->reading.size() < book->words.size())
            book->reading << true;
        // A book that is all title pages still has something to read.
        if (!book->reading.contains(true))
            book->reading.fill(true);
    }
    return book;
}

qint64 Book::wordsBefore(int section, double position) const {
    qint64 before = 0;
    for (int i = 0; i < std::min<int>(section, int(words.size())); ++i)
        if (reading.value(i, true))
            before += words[i];
    if (section >= 0 && section < words.size() && reading.value(section, true))
        before += qint64(std::clamp(position, 0.0, 1.0) * words[section]);
    return before;
}

qint64 Book::totalWords() const {
    qint64 total = 0;
    for (int i = 0; i < words.size(); ++i)
        if (reading.value(i, true))
            total += words[i];
    return total;
}

namespace {

// A fresh document for every use, as Signed does: poppler 26.08 hangs or
// aborts when one document draws a page again after drawing another, and
// loading only parses the cross-reference table, a millisecond or two.
std::unique_ptr<Poppler::Document> openPdf(const QString &path, const QByteArray &password) {
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path, password, password);
    if (!document || document->isLocked())
        return nullptr;
    document->setRenderHint(Poppler::Document::Antialiasing);
    document->setRenderHint(Poppler::Document::TextAntialiasing);
    document->setRenderHint(Poppler::Document::TextHinting);
    return document;
}

} // namespace

bool Book::loadPdf(Error *error, const QString &password) {
    m_password = password.toUtf8();
    std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path, m_password, m_password);
    if (!document) {
        *error = Error::Unreadable;
        return false;
    }
    if (document->isLocked()) {
        *error = password.isEmpty() ? Error::Locked : Error::WrongPassword;
        return false;
    }
    const QString metaTitle = document->info("Title").simplified();
    if (!metaTitle.isEmpty())
        title = metaTitle;
    author = document->info("Author").simplified();
    for (int index = 0; index < document->numPages(); ++index) {
        const std::unique_ptr<Poppler::Page> page = document->page(index);
        const QSizeF size = page ? page->pageSizeF() : QSizeF();
        aspects << (size.height() > 0 ? size.width() / size.height() : 1.0 / 1.4142);
    }

    std::function<void(const QVector<Poppler::OutlineItem> &, int)> walk = [&](const QVector<Poppler::OutlineItem> &items,
                                                                              int depth) {
        for (const Poppler::OutlineItem &item : items) {
            const auto destination = item.destination();
            const int page = destination ? destination->pageNumber() - 1 : -1;
            if (page >= 0 && page < aspects.size() && !item.name().trimmed().isEmpty())
                toc << TocEntry{item.name().simplified(), page, depth, {}};
            walk(item.children(), depth + 1);
        }
    };
    walk(document->outline(), 0);
    return true;
}

bool Book::loadEpub(Error *error) {
    QList<Entry> entries;
    if (!readArchive(path, &entries, error))
        return false;
    QHash<QString, QByteArray> files;
    for (Entry &entry : entries)
        files.insert(entry.name, std::move(entry.data));

    QXmlStreamReader container(files.value("META-INF/container.xml"));
    QString opfPath;
    while (!container.atEnd() && opfPath.isEmpty())
        if (container.readNext() == QXmlStreamReader::StartElement && container.name() == u"rootfile")
            opfPath = container.attributes().value("full-path").toString();
    if (opfPath.isEmpty() || !files.contains(opfPath)) {
        *error = Error::Unreadable;
        return false;
    }

    struct Item {
        QString path, type, properties;
    };
    QHash<QString, Item> manifest;
    QStringList spine;
    QString ncxId;
    QXmlStreamReader opf(xmlEntities(files.value(opfPath)));
    while (!opf.atEnd()) {
        if (opf.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto name = opf.name();
        const auto attributes = opf.attributes();
        if (name == u"title" && opf.namespaceUri().contains(u"purl.org/dc")) {
            const QString text = opf.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
            if (!text.isEmpty() && title == QFileInfo(path).completeBaseName())
                title = text;
        } else if (name == u"creator" && author.isEmpty()) {
            author = opf.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
        } else if (name == u"item") {
            manifest.insert(attributes.value("id").toString(),
                            {resolve(opfPath, attributes.value("href").toString()),
                             attributes.value("media-type").toString(), attributes.value("properties").toString()});
        } else if (name == u"spine") {
            ncxId = attributes.value("toc").toString();
        } else if (name == u"itemref") {
            const Item item = manifest.value(attributes.value("idref").toString());
            if (!item.path.isEmpty() && files.contains(item.path))
                spine << item.path;
        }
    }

    // The images chapters show stay in the book, drawn by the provider on demand.
    QHash<QString, QSize> imageSizes;
    QSet<QString> inkImages;
    for (const Item &item : std::as_const(manifest)) {
        if (item.type.startsWith("image/") && files.contains(item.path)) {
            m_resources.insert(item.path, files.value(item.path));
            imageSizes.insert(item.path, imageSize(files.value(item.path)));
            // A photograph has no transparent ground, so only the formats
            // that can are looked at, and only at a small size.
            if (item.type != "image/jpeg" && isInk(smallImage(files.value(item.path))))
                inkImages.insert(item.path);
        }
    }

    auto image = [&](const QString &chapter, const QString &src, const QString &alt) {
        const QString resource = resolve(chapter, decodeHtml(src));
        if (!m_resources.contains(resource))
            return alt.isEmpty() ? QString() : "<i>" + escapeAttribute(alt) + "</i>";
        const QSize size = imageSizes.value(resource);
        QString tag = "<img src=\"" + (inkImages.contains(resource) ? inkUrl(id, resource) : resourceUrl(id, resource)) + "\"";
        if (size.isValid())
            tag += QString(" width=\"%1\" height=\"%2\"").arg(size.width()).arg(size.height());
        return tag + " />";
    };

    static const QRegularExpression bodyPattern("<body[^>]*>(.*)</body>", QRegularExpression::DotMatchesEverythingOption
                                                                              | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dropPattern("<(script|style|head)\\b.*?</\\1\\s*>|<\\?xml.*?\\?>|<!--.*?-->",
                                                QRegularExpression::DotMatchesEverythingOption
                                                    | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression svgPattern("<svg\\b.*?</svg\\s*>", QRegularExpression::DotMatchesEverythingOption
                                                                           | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression svgImagePattern("<image\\b[^>]*>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression imgPattern("<img\\b[^>]*>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression linkPattern("<a\\b[^>]*>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression stylePattern("\\sstyle\\s*=\\s*(\"[^\"]*\"|'[^']*')",
                                                 QRegularExpression::CaseInsensitiveOption);

    auto replaceAll = [](const QString &text, const QRegularExpression &pattern,
                         const std::function<QString(const QRegularExpressionMatch &)> &with) {
        QString out;
        qsizetype last = 0;
        for (auto it = pattern.globalMatch(text); it.hasNext();) {
            const auto match = it.next();
            out += text.mid(last, match.capturedStart() - last) + with(match);
            last = match.capturedEnd();
        }
        return out + text.mid(last);
    };

    // Each stylesheet is read once, however many chapters link to it.
    QHash<QString, std::shared_ptr<Styling::Sheet>> sheets;
    auto sheet = [&](const QString &path) {
        auto &cached = sheets[path];
        if (!cached)
            cached = std::make_shared<Styling::Sheet>(Styling::Sheet::parse(decodeText(files.value(path))));
        return cached;
    };

    for (int index = 0; index < spine.size(); ++index) {
        const QString &chapter = spine[index];
        // The book's own styling, resolved onto each element. A chapter that
        // isn't well-formed XML is read as plain HTML, with only its inline styles.
        const QByteArray xhtml = xmlEntities(files.value(chapter));
        QStringList hrefs, inlineCss;
        Styling::linkedSheets(xhtml, &hrefs, &inlineCss);
        QList<std::shared_ptr<Styling::Sheet>> held;
        for (const QString &href : std::as_const(hrefs))
            if (const QString path = resolve(chapter, href); files.contains(path))
                held << sheet(path);
        for (const QString &css : std::as_const(inlineCss))
            held << std::make_shared<Styling::Sheet>(Styling::Sheet::parse(css));
        QList<const Styling::Sheet *> cascade;
        for (const auto &s : std::as_const(held))
            cascade << s.get();
        // Title pages, the imprint, the contents and the notes are there to
        // look at or look up, not to read through, as EPUB 3 marks them.
        {
            static const QRegularExpression marked("<(?:body|section)\\b[^>]*\\bepub:type\\s*=\\s*\"([^\"]*)\"");
            static const QStringList aside = {"titlepage", "halftitlepage", "imprint", "copyright-page", "colophon",
                                              "uncopyright", "toc", "landmarks", "loi", "lot", "endnotes", "footnotes",
                                              "rearnotes", "cover", "index"};
            bool read = true;
            int checked = 0;
            for (auto it = marked.globalMatch(QString::fromUtf8(xhtml)); it.hasNext() && checked < 2; ++checked)
                for (const QString &kind : it.next().captured(1).split(' ', Qt::SkipEmptyParts))
                    if (aside.contains(kind))
                        read = false;
            reading << read;
        }
        QHash<QString, QString> chapterNotes;
        QString html = Styling::apply(xhtml, cascade, &chapterNotes);
        const bool styled = !html.isNull();
        if (!styled) {
            html = decodeText(files.value(chapter));
            const auto body = bodyPattern.match(html);
            if (body.hasMatch())
                html = body.captured(1);
            html.remove(dropPattern);
        }
        // Images, SVG covers and links, the same in the chapter and its notes.
        auto finish = [&](QString text) {
            // Images first, so the ones the SVG pass makes aren't rewritten again.
            text = replaceAll(text, imgPattern, [&](const QRegularExpressionMatch &match) {
                return image(chapter, attribute(match.captured(), "src"), decodeHtml(attribute(match.captured(), "alt")));
            });
            // A cover is often an SVG wrapping one image: keep only the image.
            text = replaceAll(text, svgPattern, [&](const QRegularExpressionMatch &svg) {
                QString images;
                for (auto it = svgImagePattern.globalMatch(svg.captured()); it.hasNext();) {
                    const QString tag = it.next().captured();
                    QString href = attribute(tag, "xlink:href");
                    if (href.isEmpty())
                        href = attribute(tag, "href");
                    images += image(chapter, href, {});
                }
                return images;
            });
            // Links to other chapters are followed inside the reader; the rest
            // open in the browser. An anchor keeps its id, which a link or the
            // contents may point at, and its style, as a footnote's raised number has.
            text = replaceAll(text, linkPattern, [&](const QRegularExpressionMatch &match) {
                const QString tag = match.captured();
                QString kept;
                for (const QString name : {"id", "name", "style"})
                    if (const QString value = attribute(tag, name); !value.isEmpty())
                        kept += ' ' + name + "=\"" + value + '"';
                const QString href = decodeHtml(attribute(tag, "href"));
                if (href.isEmpty())
                    return "<a" + kept + ">";
                if (!QUrl(href).scheme().isEmpty())
                    return "<a href=\"" + escapeAttribute(href) + "\"" + kept + ">";
                const int target = int(spine.indexOf(resolve(chapter, href)));
                const QString fragment = QUrl(href).fragment(QUrl::FullyDecoded);
                return target < 0 ? "<a" + kept + ">"
                                  : QString("<a href=\"omaread:%1%2\"%3>")
                                        .arg(target)
                                        .arg(fragment.isEmpty() ? QString() : "#" + escapeAttribute(fragment), kept);
            });
            if (!styled)
                text = replaceAll(text, stylePattern, [&](const QRegularExpressionMatch &match) {
                    const QString style = keptStyle(decodeHtml(match.captured(1).mid(1, match.captured(1).size() - 2)));
                    return style.isEmpty() ? QString() : " style=\"" + escapeAttribute(style) + "\"";
                });
            return text;
        };
        html = finish(html);
        for (auto it = chapterNotes.cbegin(); it != chapterNotes.cend(); ++it)
            notes.insert(QString("%1#%2").arg(index).arg(it.key()), finish(it.value()));
        sections << html.trimmed();
        aspects << 0;
    }

    // The table of contents: EPUB 3's nav document, or EPUB 2's NCX.
    QString navPath, ncxPath = manifest.value(ncxId).path;
    for (const Item &item : std::as_const(manifest)) {
        if (item.properties.split(' ').contains("nav"))
            navPath = item.path;
        if (ncxPath.isEmpty() && item.type == "application/x-dtbncx+xml")
            ncxPath = item.path;
    }
    auto addEntry = [&](const QString &from, const QString &entryTitle, const QString &href, int depth) {
        const int section = int(spine.indexOf(resolve(from, href)));
        if (section >= 0 && !entryTitle.isEmpty())
            toc << TocEntry{entryTitle, section, std::max(0, depth), QUrl(href).fragment(QUrl::FullyDecoded)};
    };
    if (!navPath.isEmpty()) {
        QXmlStreamReader nav(xmlEntities(files.value(navPath)));
        int navDepth = 0, listDepth = 0;
        bool inToc = false;
        while (!nav.atEnd()) {
            const auto token = nav.readNext();
            if (token == QXmlStreamReader::StartElement) {
                if (nav.name() == u"nav") {
                    ++navDepth;
                    const QString type = nav.attributes().value("http://www.idpf.org/2007/ops", "type").toString();
                    inToc = type.split(' ').contains("toc");
                } else if (inToc && nav.name() == u"ol") {
                    ++listDepth;
                } else if (inToc && nav.name() == u"a") {
                    const QString href = nav.attributes().value("href").toString();
                    addEntry(navPath, nav.readElementText(QXmlStreamReader::IncludeChildElements).simplified(), href,
                             listDepth - 1);
                }
            } else if (token == QXmlStreamReader::EndElement) {
                if (nav.name() == u"nav" && --navDepth == 0)
                    inToc = false;
                else if (inToc && nav.name() == u"ol")
                    --listDepth;
            }
        }
    }
    if (toc.isEmpty() && !ncxPath.isEmpty()) {
        QXmlStreamReader ncx(xmlEntities(files.value(ncxPath)));
        int depth = -1;
        QString label;
        while (!ncx.atEnd()) {
            const auto token = ncx.readNext();
            if (token == QXmlStreamReader::StartElement) {
                if (ncx.name() == u"navPoint")
                    ++depth;
                else if (ncx.name() == u"text" && depth >= 0)
                    label = ncx.readElementText().simplified();
                else if (ncx.name() == u"content" && depth >= 0)
                    addEntry(ncxPath, label, ncx.attributes().value("src").toString(), depth);
            } else if (token == QXmlStreamReader::EndElement && ncx.name() == u"navPoint") {
                --depth;
            }
        }
    }
    return true;
}

bool Book::loadComic(Error *error) {
    QList<Entry> entries;
    if (!readArchive(path, &entries, error))
        return false;
    entries.removeIf([](const Entry &entry) {
        const QString name = QFileInfo(entry.name).fileName();
        return !isImageName(entry.name) || name.startsWith('.') || entry.name.startsWith("__MACOSX/");
    });
    // Pages are numbered by name, and page10 comes after page9.
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(entries.begin(), entries.end(),
              [&](const Entry &a, const Entry &b) { return collator.compare(a.name, b.name) < 0; });
    for (Entry &entry : entries) {
        const QSize size = imageSize(entry.data);
        if (!size.isValid())
            continue;
        aspects << double(size.width()) / size.height();
        m_pages << std::move(entry.data);
    }
    return true;
}

bool Book::loadMarkdown(Error *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = Error::Unreadable;
        return false;
    }
    const QString text = decodeText(file.readAll());
    static const QRegularExpression heading("^(#{1,2})[ \\t]+(.+?)[ \\t#]*$");
    static const QRegularExpression fence("^\\s*(```|~~~)");
    // A chapter starts at every first- and second-level heading outside code.
    // Headings with nothing under them yet, such as a book's `# Title` right
    // before its first `## Chapter`, stay with the chapter that follows rather
    // than making a page of their own with only a heading on it.
    QString current;
    bool inFence = false, titled = false, hasBody = false;
    for (const QString &line : text.split('\n')) {
        if (fence.match(line).hasMatch())
            inFence = !inFence;
        const auto match = inFence ? QRegularExpressionMatch() : heading.match(line);
        if (match.hasMatch()) {
            if (hasBody) {
                sections << current;
                aspects << 0;
                current.clear();
                hasBody = false;
            }
            toc << TocEntry{match.captured(2).trimmed(), int(sections.size()), int(match.captured(1).size()) - 1, {}};
            if (!titled && match.captured(1) == "#") {
                title = match.captured(2).trimmed();
                titled = true;
            }
        } else if (!line.trimmed().isEmpty()) {
            hasBody = true;
        }
        current += line + '\n';
    }
    if (!current.trimmed().isEmpty() || sections.isEmpty()) {
        sections << current;
        aspects << 0;
    }
    // A heading at the very end, with nothing under it, has no section.
    toc.removeIf([&](const TocEntry &entry) { return entry.section >= sections.size(); });
    return true;
}

bool Book::loadText(Error *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = Error::Unreadable;
        return false;
    }
    const QString text = decodeText(file.readAll()).replace("\r\n", "\n");
    // Cut at paragraph breaks, never inside one.
    QString current;
    for (const QString &paragraph : text.split("\n\n")) {
        if (!current.isEmpty() && current.size() + paragraph.size() > textSectionLength) {
            sections << current;
            aspects << 0;
            current.clear();
        }
        current += (current.isEmpty() ? "" : "\n\n") + paragraph;
    }
    if (!current.isEmpty() || sections.isEmpty()) {
        sections << current;
        aspects << 0;
    }
    return true;
}

QString Book::kindName() const {
    switch (kind) {
    case Kind::Pdf: return "pdf";
    case Kind::Epub: return "epub";
    case Kind::Comic: return "comic";
    case Kind::Markdown: return "markdown";
    case Kind::Text: return "text";
    }
    return "text";
}

int Book::sectionForLink(const QString &link, QString *fragment) const {
    if (!link.startsWith("omaread:"))
        return -1;
    const QString target = link.mid(8);
    bool ok = false;
    const int section = target.section('#', 0, 0).toInt(&ok);
    if (fragment)
        *fragment = target.contains('#') ? decodeHtml(target.section('#', 1)) : QString();
    return ok && section >= 0 && section < sectionCount() ? section : -1;
}

int Book::fragmentOffset(int section, const QString &fragment) const {
    if (fragment.isEmpty() || paged() || section < 0 || section >= sections.size() || kind != Kind::Epub)
        return -1;
    const QString &html = sections[section];
    const QRegularExpression target("<(\\w+)[^>]*\\b(id|name)\\s*=\\s*([\"'])" + QRegularExpression::escape(fragment) + "\\3",
                                    QRegularExpression::CaseInsensitiveOption);
    const auto match = target.match(html);
    if (!match.hasMatch())
        return -1;
    // The text before the element is what the window lays out before it, and
    // an element that starts a block of its own comes after one more break.
    static const QStringList blocks = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "li",
                                       "blockquote", "pre", "table", "tr", "ul", "ol", "header", "aside", "figure"};
    QTextDocument document;
    document.setHtml(html.left(match.capturedStart()));
    const int before = int(document.toPlainText().size());
    return before + (before > 0 && blocks.contains(match.captured(1).toLower()) ? 1 : 0);
}

QString Book::plainText(int section) const {
    if (section < 0 || section >= sectionCount())
        return {};
    switch (kind) {
    case Kind::Pdf: {
        QMutexLocker lock(&m_pdfMutex);
        const auto document = openPdf(path, m_password);
        const auto page = document ? document->page(section) : nullptr;
        return page ? page->text(QRectF()) : QString();
    }
    case Kind::Comic: return {};
    case Kind::Epub: {
        QTextDocument document;
        document.setHtml(sections[section]);
        return document.toPlainText();
    }
    case Kind::Markdown: {
        QTextDocument document;
        document.setMarkdown(sections[section]);
        return document.toPlainText();
    }
    case Kind::Text: return sections[section];
    }
    return {};
}

QString Book::pageText(int index, const QRectF &area) const {
    if (kind != Kind::Pdf || index < 0 || index >= aspects.size())
        return {};
    QMutexLocker lock(&m_pdfMutex);
    const auto document = openPdf(path, m_password);
    const auto page = document ? document->page(index) : nullptr;
    if (!page)
        return {};
    const QSizeF size = page->pageSizeF();
    return page->text(QRectF(area.x() * size.width(), area.y() * size.height(), area.width() * size.width(),
                             area.height() * size.height()))
        .trimmed();
}

QList<PageLink> Book::pageLinks(int index) const {
    QList<PageLink> links;
    if (kind != Kind::Pdf || index < 0 || index >= aspects.size())
        return links;
    QMutexLocker lock(&m_pdfMutex);
    const auto document = openPdf(path, m_password);
    const auto page = document ? document->page(index) : nullptr;
    if (!page)
        return links;
    for (const auto &link : page->links()) {
        PageLink found{link->linkArea().normalized(), -1, {}};
        if (link->linkType() == Poppler::Link::Goto) {
            const auto *go = static_cast<const Poppler::LinkGoto *>(link.get());
            if (!go->isExternal())
                found.page = go->destination().pageNumber() - 1;
        } else if (link->linkType() == Poppler::Link::Browse) {
            found.url = static_cast<const Poppler::LinkBrowse *>(link.get())->url();
        }
        if ((found.page >= 0 && found.page < aspects.size()) || !found.url.isEmpty())
            links << found;
    }
    return links;
}

QList<SearchHit> Book::search(const QString &words, const std::atomic_bool *cancel) const {
    QList<SearchHit> hits;
    const QString needle = words.simplified();
    if (needle.isEmpty())
        return hits;
    if (kind == Kind::Pdf) {
        QMutexLocker lock(&m_pdfMutex);
        const auto document = openPdf(path, m_password);
        for (int index = 0; document && index < document->numPages(); ++index) {
            if (cancel && *cancel)
                break;
            const auto page = document->page(index);
            if (!page)
                continue;
            const QSizeF size = page->pageSizeF();
            int occurrence = 0;
            for (const QRectF &rect : page->search(needle, Poppler::Page::IgnoreCase | Poppler::Page::IgnoreDiacritics))
                hits << SearchHit{index, occurrence++,
                                  QRectF(rect.x() / size.width(), rect.y() / size.height(), rect.width() / size.width(),
                                         rect.height() / size.height())};
        }
        return hits;
    }
    for (int section = 0; section < sections.size(); ++section) {
        if (cancel && *cancel)
            break;
        // The window lays text out with paragraph separators where this has
        // line breaks; both are one character, so the counts line up.
        const QString text = plainText(section);
        int occurrence = 0;
        for (qsizetype at = text.indexOf(needle, 0, Qt::CaseInsensitive); at >= 0;
             at = text.indexOf(needle, at + needle.size(), Qt::CaseInsensitive))
            hits << SearchHit{section, occurrence++, {}};
    }
    return hits;
}

QImage Book::renderPage(int page, const QSize &requested, const QColor &background, const QColor &foreground) const {
    if (page < 0 || page >= aspects.size())
        return {};
    const double aspect = aspects[page];
    QSize size = requested;
    if (!size.isValid() || size.isEmpty())
        size = QSize(1200, int(1200 / aspect));
    else
        size = QSizeF(aspect, 1).scaled(QSizeF(size), Qt::KeepAspectRatio).toSize();
    size = size.boundedTo(QSize(8000, 8000)).expandedTo(QSize(1, 1));

    QImage image;
    if (kind == Kind::Pdf) {
        QMutexLocker lock(&m_pdfMutex);
        const auto document = openPdf(path, m_password);
        const auto pdfPage = document ? document->page(page) : nullptr;
        if (pdfPage) {
            const double dpi = 72.0 * size.width() / pdfPage->pageSizeF().width();
            image = pdfPage->renderToImage(dpi, dpi).convertToFormat(QImage::Format_RGB32);
        }
    } else if (page < m_pages.size()) {
        image = QImage::fromData(m_pages[page]);
        if (!image.isNull())
            image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    }
    if (image.isNull() || !background.isValid() || !foreground.isValid())
        return image;

    // A tint keeps the page's lightness and swaps its colors for the theme's:
    // white paper becomes the background and black ink the foreground.
    QRgb ramp[256];
    for (int level = 0; level < 256; ++level) {
        const double t = level / 255.0;
        ramp[level] = qRgb(int(foreground.red() + (background.red() - foreground.red()) * t),
                           int(foreground.green() + (background.green() - foreground.green()) * t),
                           int(foreground.blue() + (background.blue() - foreground.blue()) * t));
    }
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            line[x] = ramp[qGray(line[x])];
    }
    return image;
}

QImage Book::resource(const QString &resourcePath) const {
    const auto it = m_resources.constFind(resourcePath);
    return it == m_resources.cend() ? QImage() : QImage::fromData(*it);
}

QString Book::resourceUrl(int bookId, const QString &resourcePath) {
    return QString("image://omaread/res/%1/%2").arg(bookId).arg(QString::fromLatin1(resourcePath.toUtf8().toHex()));
}

QString Book::inkUrl(int bookId, const QString &resourcePath) {
    return QString("image://omaread/ink/%1/%2").arg(bookId).arg(QString::fromLatin1(resourcePath.toUtf8().toHex()));
}

QString Book::withInk(const QString &html, const QColor &ink) {
    if (!ink.isValid())
        return html;
    static const QRegularExpression url("(image://omaread/ink/\\d+/[0-9a-f]+)\"");
    QString out = html;
    return out.replace(url, "\\1/" + ink.name().mid(1) + "\"");
}

bool Book::isInk(const QImage &source) {
    if (source.isNull() || !source.hasAlphaChannel())
        return false;
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    qsizetype clear = 0, dark = 0, light = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            if (qAlpha(pixel) < 16) {
                ++clear;
                continue;
            }
            const int high = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
            const int low = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
            // Grey only: a drawing in color stays as it is.
            if (high - low > 40)
                return false;
            ++(high < 128 ? dark : light);
        }
    }
    // Mostly ground, and drawn mostly in dark strokes. A light edge around
    // them, as Standard Ebooks' lettering has, fades out when inked.
    return dark > 0 && dark >= light && clear >= qsizetype(image.width()) * image.height() / 4;
}

QImage Book::inked(const QImage &source, const QColor &ink) {
    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const QRgb color = ink.rgb();
    for (int y = 0; y < image.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            // Darker strokes stay stronger: grey anti-aliasing fades out.
            const int darkness = 255 - qGray(line[x]);
            line[x] = qRgba(qRed(color), qGreen(color), qBlue(color), qAlpha(line[x]) * darkness / 255);
        }
    }
    return image;
}

QString Book::fitImages(const QString &html, int width) {
    static const QRegularExpression sized("<img src=\"([^\"]*)\" width=\"(\\d+)\" height=\"(\\d+)\"");
    QString out;
    qsizetype last = 0;
    if (width > 0) {
        for (auto it = sized.globalMatch(html); it.hasNext();) {
            const auto match = it.next();
            const int w = match.captured(2).toInt(), h = match.captured(3).toInt();
            out += html.mid(last, match.capturedStart() - last);
            if (w > width && w > 0)
                out += QString("<img src=\"%1\" width=\"%2\" height=\"%3\"").arg(match.captured(1)).arg(width).arg(
                    qMax(1, int(double(h) * width / w)));
            else
                out += match.captured();
            last = match.capturedEnd();
        }
    }
    out += html.mid(last);
    // A centered block holding only an image (see Styling): the left margin
    // that centers the image in the column.
    static const QRegularExpression centered(
        "margin-left: centered(\"[^>]*>\\s*<img src=\"[^\"]*\" width=\"(\\d+)\")");
    QString result;
    last = 0;
    for (auto it = centered.globalMatch(out); it.hasNext();) {
        const auto match = it.next();
        const int margin = width > 0 ? std::max(0, (width - match.captured(2).toInt()) / 2) : 0;
        result += out.mid(last, match.capturedStart() - last) + QString("margin-left: %1px").arg(margin)
                  + match.captured(1);
        last = match.capturedEnd();
    }
    result += out.mid(last);
    return result.replace("margin-left: centered", "margin-left: 0px");
}
