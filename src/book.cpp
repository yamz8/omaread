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
#include <QtConcurrent>

#include <poppler-qt6.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <numeric>

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
    // A plain scan rather than a pattern made for each name: this is asked of
    // every link and image in a book.
    qsizetype at = 0;
    while ((at = tag.indexOf(name, at, Qt::CaseInsensitive)) >= 0) {
        const qsizetype end = at + name.size();
        const bool starts = at == 0 || !(tag[at - 1].isLetterOrNumber() || tag[at - 1] == '-' || tag[at - 1] == ':');
        qsizetype i = end;
        while (i < tag.size() && tag[i].isSpace())
            ++i;
        if (starts && i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() && tag[i].isSpace())
                ++i;
            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                const QChar quote = tag[i];
                const qsizetype close = tag.indexOf(quote, i + 1);
                if (close > i)
                    return tag.mid(i + 1, close - i - 1);
            }
        }
        at = end;
    }
    return {};
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
    book->title = titleFromFileName(path);

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
        // Words are runs of letters and digits, outside the markup's tags,
        // counted a chapter to a core.
        const bool markup = book->kind == Kind::Epub;
        book->words = QtConcurrent::blockingMapped(book->sections, [markup](const QString &section) {
            int count = 0;
            bool inTag = false, inWord = false;
            for (const QChar c : section) {
                if (markup && c == '<') {
                    inTag = true;
                    inWord = false;
                } else if (inTag) {
                    if (c == '>')
                        inTag = false;
                } else if (c.isLetterOrNumber()) {
                    if (!inWord)
                        ++count;
                    inWord = true;
                } else if (!(inWord && (c == '\'' || c == QChar(0x2019) || c == '-' || c.isMark()))) {
                    inWord = false;
                }
            }
            return count;
        });
        while (book->reading.size() < book->words.size())
            book->reading << true;
        // A book that is all title pages still has something to read.
        if (!book->reading.contains(true))
            book->reading.fill(true);
    }
    return book;
}

QString Book::chapterLabel(int section) const {
    int index = -1;
    for (int i = 0; i < toc.size(); ++i)
        if (toc[i].section <= section)
            index = i;
    if (index < 0)
        return {};
    // The entry and the ones it sits under, nearest first.
    QStringList chain;
    int depth = toc[index].depth + 1;
    for (int i = index; i >= 0 && depth > 0; --i) {
        const TocEntry &entry = toc[i];
        if (entry.depth >= depth)
            continue;
        depth = entry.depth;
        if (entry.title.compare(title, Qt::CaseInsensitive) != 0)
            chain.prepend(entry.title);
    }
    while (chain.size() > 2)
        chain.removeFirst();
    return chain.join(QStringLiteral("  ·  "));
}

QString Book::notesMarkdown(const QList<Highlight> &highlights) const {
    QString out = "# " + title + '\n';
    if (!author.isEmpty())
        out += '\n' + author + '\n';
    QString chapter = QStringLiteral("\x01");
    for (const Highlight &highlight : highlights) {
        const QString label = chapterLabel(highlight.section).replace(QStringLiteral("  ·  "), QStringLiteral(" · "));
        if (label != chapter) {
            chapter = label;
            if (!label.isEmpty())
                out += "\n## " + label + '\n';
        }
        // A quotation, line by line, and the note under it.
        QString quote;
        for (const QString &line : highlight.text.split(QRegularExpression("[\\n\\x{2029}\\x{2028}]+"), Qt::SkipEmptyParts))
            quote += "> " + line.simplified() + '\n';
        out += '\n' + quote;
        if (!highlight.note.trimmed().isEmpty())
            out += '\n' + highlight.note.trimmed() + '\n';
    }
    return out;
}

QRectF Book::contentBox(int page) const {
    if (!paged() || page < 0 || page >= aspects.size())
        return QRectF(0, 0, 1, 1);
    return contentBox(renderPage(page, QSize(400, 400)));
}

QRectF Book::contentBox(const QImage &source) {
    const QRectF whole(0, 0, 1, 1);
    if (source.isNull())
        return whole;
    const QImage image = source.convertToFormat(QImage::Format_Grayscale8);
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
            if (line[x] < 235) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    }
    if (right < left || bottom < top)
        return whole;
    // A little room around the words, as a page's own margin would leave.
    const double pad = 0.02;
    QRectF box(double(left) / image.width() - pad, double(top) / image.height() - pad,
               double(right - left + 1) / image.width() + 2 * pad, double(bottom - top + 1) / image.height() + 2 * pad);
    box = box.intersected(whole);
    // Hardly any margin: nothing worth trimming.
    return box.width() * box.height() > 0.94 ? whole : box;
}

QList<QRectF> Book::panels(int page, bool rightToLeft) const {
    if (!paged() || page < 0 || page >= aspects.size())
        return {};
    return findPanels(renderPage(page, QSize(4000, 900)), rightToLeft);
}

// The page is cut where a whole row, then a whole column, is the gutters'
// color, again inside each piece, a few levels down: how comic pages are
// laid out, in strips of panels.
QList<QRectF> Book::findPanels(const QImage &source, bool rightToLeft) {
    if (source.isNull() || source.width() < 40 || source.height() < 40)
        return {};
    const QImage image = source.convertToFormat(QImage::Format_Grayscale8);
    const int width = image.width(), height = image.height();
    // The gutters' color: what most of the page's edge is.
    QList<int> edge;
    for (int x = 0; x < width; x += 2) {
        edge << image.constScanLine(1)[x] << image.constScanLine(height - 2)[x];
    }
    for (int y = 0; y < height; y += 2) {
        edge << image.constScanLine(y)[1] << image.constScanLine(y)[width - 2];
    }
    std::sort(edge.begin(), edge.end());
    const int ground = edge[edge.size() / 2];
    auto ink = [&](int x, int y) { return std::abs(int(image.constScanLine(y)[x]) - ground) > 48; };

    struct Box {
        int left, top, right, bottom;    // right and bottom exclusive
    };
    const int gap = std::max(4, height / 80);
    // The stretches along one axis of a box that have ink, split where a run of
    // empty lines at least gap long lies between them.
    auto stretches = [&](const Box &box, bool rows) {
        const int from = rows ? box.top : box.left, to = rows ? box.bottom : box.right;
        const int across = rows ? box.right - box.left : box.bottom - box.top;
        const int allowed = std::max(1, across / 160);
        QList<std::pair<int, int>> out;
        int start = -1, empty = 0;
        for (int i = from; i < to; ++i) {
            int count = 0;
            for (int j = rows ? box.left : box.top; j < (rows ? box.right : box.bottom) && count <= allowed; ++j)
                count += rows ? ink(j, i) : ink(i, j);
            if (count > allowed) {
                if (start < 0)
                    start = i;
                else if (empty >= gap) {
                    out << std::pair{start, i - empty};
                    start = i;
                }
                empty = 0;
            } else if (start >= 0) {
                ++empty;
            }
        }
        if (start >= 0)
            out << std::pair{start, to - empty};
        return out;
    };
    QList<Box> found;
    std::function<void(const Box &, int)> cut = [&](const Box &box, int depth) {
        const auto rows = stretches(box, true);
        if (depth < 4 && rows.size() > 1) {
            for (const auto &[top, bottom] : rows)
                cut({box.left, top, box.right, bottom}, depth + 1);
            return;
        }
        auto columns = stretches(box, false);
        if (depth < 4 && columns.size() > 1) {
            if (rightToLeft)
                std::reverse(columns.begin(), columns.end());
            for (const auto &[left, right] : columns)
                cut({left, box.top, right, box.bottom}, depth + 1);
            return;
        }
        const Box inked{columns.isEmpty() ? box.left : columns.first().first, rows.isEmpty() ? box.top : rows.first().first,
                        columns.isEmpty() ? box.right : columns.last().second, rows.isEmpty() ? box.bottom : rows.last().second};
        if (inked.right > inked.left && inked.bottom > inked.top)
            found << inked;
    };
    cut({0, 0, width, height}, 0);
    // Specks between panels aren't panels.
    found.removeIf([&](const Box &b) { return (b.right - b.left) * (b.bottom - b.top) < width * height / 60; });
    if (found.size() < 2)
        return {};
    QList<QRectF> panels;
    for (const Box &b : std::as_const(found))
        panels << QRectF(double(b.left) / width, double(b.top) / height, double(b.right - b.left) / width,
                         double(b.bottom - b.top) / height);
    return panels;
}

QImage Book::cover(int height) const {
    QImage image;
    switch (kind) {
    case Kind::Epub: image = resource(m_coverPath); break;
    case Kind::Pdf:
    case Kind::Comic: image = renderPage(0, QSize(height * 2, height)); break;
    default: break;
    }
    if (!image.isNull() && image.height() > height)
        image = image.scaledToHeight(height, Qt::SmoothTransformation);
    return image;
}

QString Book::titleFromFileName(const QString &path) {
    QString name = QFileInfo(path).completeBaseName();
    name.replace('_', ' ').replace(QRegularExpression("(?<=\\S)-(?=\\S)"), " ");
    name = name.simplified();
    // All in lower case, as file names often are: each word capitalized.
    if (name == name.toLower()) {
        QStringList words = name.split(' ');
        for (QString &word : words)
            if (!word.isEmpty())
                word[0] = word[0].toUpper();
        name = words.join(' ');
    }
    return name.isEmpty() ? QFileInfo(path).fileName() : name;
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
    QString ncxId, coverId;
    QXmlStreamReader opf(xmlEntities(files.value(opfPath)));
    while (!opf.atEnd()) {
        if (opf.readNext() != QXmlStreamReader::StartElement)
            continue;
        const auto name = opf.name();
        const auto attributes = opf.attributes();
        if (name == u"title" && opf.namespaceUri().contains(u"purl.org/dc")) {
            const QString text = opf.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
            if (!text.isEmpty() && title == titleFromFileName(path))
                title = text;
        } else if (name == u"meta" && attributes.value("name") == u"cover") {
            coverId = attributes.value("content").toString();
        } else if (name == u"creator" && author.isEmpty()) {
            author = opf.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
        } else if (name == u"item") {
            manifest.insert(attributes.value("id").toString(),
                            {resolve(opfPath, attributes.value("href").toString()),
                             attributes.value("media-type").toString(), attributes.value("properties").toString()});
        } else if (name == u"spine") {
            ncxId = attributes.value("toc").toString();
            rightToLeft = attributes.value("page-progression-direction") == u"rtl";
        } else if (name == u"itemref") {
            const Item item = manifest.value(attributes.value("idref").toString());
            if (!item.path.isEmpty() && files.contains(item.path))
                spine << item.path;
        }
    }
    // The cover: EPUB 3 marks it, EPUB 2 names it in a meta, and some books
    // only call the file so.
    for (auto it = manifest.cbegin(); it != manifest.cend() && m_coverPath.isEmpty(); ++it)
        if (it->properties.split(' ').contains("cover-image") && files.contains(it->path))
            m_coverPath = it->path;
    if (m_coverPath.isEmpty() && manifest.contains(coverId) && files.contains(manifest.value(coverId).path))
        m_coverPath = manifest.value(coverId).path;
    for (auto it = manifest.cbegin(); it != manifest.cend() && m_coverPath.isEmpty(); ++it)
        if (it->type.startsWith("image/") && QFileInfo(it->path).completeBaseName().toLower().contains("cover")
            && files.contains(it->path))
            m_coverPath = it->path;

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

    // Each stylesheet is read once, however many chapters link to it, and
    // each set of them made ready once, however many chapters share it.
    QHash<QString, std::shared_ptr<Styling::Sheet>> sheets;
    QHash<QString, std::shared_ptr<const Styling::Rules>> compiled;
    auto sheet = [&](const QString &path) {
        auto &cached = sheets[path];
        if (!cached)
            cached = std::make_shared<Styling::Sheet>(Styling::Sheet::parse(decodeText(files.value(path))));
        return cached;
    };

    // Each chapter's sheets, made ready one after another, since those are
    // shared between chapters; then the chapters themselves, all at once.
    struct Prepared {
        QByteArray xhtml;
        std::shared_ptr<const Styling::Rules> rules;
        QList<std::shared_ptr<Styling::Sheet>> held;
    };
    QList<Prepared> prepared;
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
        std::shared_ptr<const Styling::Rules> rules;
        if (inlineCss.isEmpty()) {
            const QString key = hrefs.join('\n') + '|' + QFileInfo(chapter).path();
            auto &known = compiled[key];
            if (!known)
                known = Styling::compile(cascade);
            rules = known;
        } else {
            rules = Styling::compile(cascade);
        }
        prepared << Prepared{xhtml, rules, held};
    }
    QHash<QString, int> spineIndex;
    for (int index = 0; index < spine.size(); ++index)
        spineIndex.insert(spine[index], index);

    struct Done {
        QString html;
        QHash<QString, QString> notes;
        bool read = true;
    };
    QList<int> order(spine.size());
    std::iota(order.begin(), order.end(), 0);
    const QList<Done> done = QtConcurrent::blockingMapped(order, [&](int index) {
        const QString &chapter = spine[index];
        const QByteArray &xhtml = prepared[index].xhtml;
        const Styling::Rules &rules = *prepared[index].rules;
        Done result;
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
            result.read = read;
        }
        QHash<QString, QString> chapterNotes;
        QString html = Styling::apply(xhtml, rules, &chapterNotes);
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
                const int target = spineIndex.value(resolve(chapter, href), -1);
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
        result.html = finish(html).trimmed();
        for (auto it = chapterNotes.cbegin(); it != chapterNotes.cend(); ++it)
            result.notes.insert(QString("%1#%2").arg(index).arg(it.key()), finish(it.value()));
        return result;
    });
    for (const Done &chapter : done) {
        sections << chapter.html;
        aspects << 0;
        reading << chapter.read;
        notes.insert(chapter.notes);
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
    // ComicInfo.xml, as comic readers and taggers write it: the series and
    // number, the writer, and whether it's a manga read right to left.
    for (const Entry &entry : std::as_const(entries)) {
        if (QFileInfo(entry.name).fileName().compare("ComicInfo.xml", Qt::CaseInsensitive) != 0)
            continue;
        QXmlStreamReader xml(entry.data);
        QString series, number, name;
        while (!xml.atEnd()) {
            if (xml.readNext() != QXmlStreamReader::StartElement)
                continue;
            const auto element = xml.name();
            if (element == u"Series")
                series = xml.readElementText().simplified();
            else if (element == u"Number")
                number = xml.readElementText().simplified();
            else if (element == u"Title")
                name = xml.readElementText().simplified();
            else if (element == u"Writer" && author.isEmpty())
                author = xml.readElementText().simplified();
            else if (element == u"Manga")
                rightToLeft = xml.readElementText().simplified() == u"YesAndRightToLeft";
        }
        if (!series.isEmpty())
            title = series + (number.isEmpty() ? QString() : " " + number) + (name.isEmpty() ? QString() : ": " + name);
        else if (!name.isEmpty())
            title = name;
        break;
    }
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
                                         rect.height() / size.height()),
                                  -1, {}, 0};
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
             at = text.indexOf(needle, at + needle.size(), Qt::CaseInsensitive)) {
            // Words that begin with what was asked for, as a reader means it:
            // "lamp" finds "lamps", but "axe" never finds "waxed".
            if (at > 0 && (text[at - 1].isLetterOrNumber() || text[at - 1] == '\''
                           || text[at - 1] == QChar(0x2019)) && needle[0].isLetterOrNumber())
                continue;
            SearchHit hit{section, occurrence++, {}, int(at), {}, 0};
            // The words around it, cut at spaces, on one line.
            qsizetype from = std::max<qsizetype>(0, at - 60), to = std::min(text.size(), at + needle.size() + 90);
            if (from > 0)
                if (const qsizetype space = text.indexOf(' ', from); space >= 0 && space < at)
                    from = space + 1;
            if (to < text.size())
                if (const qsizetype space = text.lastIndexOf(' ', to); space > at + needle.size())
                    to = space;
            QString context = text.mid(from, to - from);
            for (QChar &c : context)
                if (c == '\n' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator || c == '\t')
                    c = ' ';
            hit.context = (from > 0 ? QStringLiteral("…") : QString()) + context + (to < text.size() ? QStringLiteral("…") : QString());
            hit.contextAt = int(at - from) + (from > 0 ? 1 : 0);
            hits << hit;
        }
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
