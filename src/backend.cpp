#include "backend.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtConcurrent>

#include <algorithm>

#include "portalfilepicker.h"

namespace {

// Enough to find any recent book again without the list becoming an archive.
constexpr int maxLibrary = 60;

QString defaultStateDirectory() {
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    return state + "/omaread";
}

QVariantMap area(int page, const QRectF &rect) {
    return {{"page", page}, {"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
}

} // namespace

Backend::Backend(const QString &stateDirectory, FilePicker *picker, QObject *parent)
    : QObject(parent),
      m_stateFile((stateDirectory.isEmpty() ? defaultStateDirectory() : stateDirectory) + "/library.json"),
      m_picker(picker ? picker : new PortalFilePicker(this)) {
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);
    connect(m_picker, &FilePicker::openSelected, this, &Backend::openUrl);
    connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setProblem(Problem::Picker, message); });
    readState();
}

Backend::~Backend() {
    if (m_searchCancel)
        *m_searchCancel = true;
    if (m_saveTimer.isActive())
        save();
}

void Backend::readState() {
    QFile file(m_stateFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    m_textScale = std::clamp(state.value("textScale").toDouble(defaultTextScale), 0.8, 4.0);
    m_tintPages = state.value("tintPages").toBool();
    m_spread = state.value("spread").toBool();
    for (const QJsonValue &value : state.value("books").toArray()) {
        const QJsonObject book = value.toObject();
        Entry entry{book.value("path").toString(), book.value("title").toString(), book.value("author").toString(),
                    book.value("kind").toString(), book.value("section").toInt(), book.value("sections").toInt(),
                    std::clamp(book.value("position").toDouble(), 0.0, 1.0), qint64(book.value("opened").toDouble())};
        if (!entry.path.isEmpty())
            m_library << entry;
    }
}

void Backend::save() {
    m_saveTimer.stop();
    QJsonArray books;
    for (const Entry &entry : std::as_const(m_library))
        books.append(QJsonObject{{"path", entry.path},         {"title", entry.title},
                                 {"author", entry.author},     {"kind", entry.kind},
                                 {"section", entry.section},   {"sections", entry.sections},
                                 {"position", entry.position}, {"opened", double(entry.opened)}});
    const QJsonObject state{
        {"textScale", m_textScale}, {"tintPages", m_tintPages}, {"spread", m_spread}, {"books", books}};
    QDir().mkpath(QFileInfo(m_stateFile).path());
    // Atomic, so a crash mid-write never loses the whole library.
    QSaveFile file(m_stateFile);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(state).toJson());
        file.commit();
    }
}

void Backend::scheduleSave() {
    m_saveTimer.start();
}

QVariantList Backend::library() const {
    QVariantList list;
    for (const Entry &entry : m_library) {
        const double progress =
            entry.sections <= 0 ? 0
            : entry.kind == "pdf" || entry.kind == "comic"
                ? double(entry.section + 1) / entry.sections
                : (entry.section + entry.position) / entry.sections;
        list << QVariantMap{{"path", entry.path},
                            {"title", entry.title.isEmpty() ? QFileInfo(entry.path).completeBaseName() : entry.title},
                            {"author", entry.author},
                            {"kind", entry.kind},
                            {"fileName", QFileInfo(entry.path).fileName()},
                            {"progress", std::clamp(progress, 0.0, 1.0)},
                            {"exists", QFileInfo::exists(entry.path)}};
    }
    return list;
}

QString Backend::error() const {
    const QString name = QFileInfo(m_problemDetail).fileName();
    switch (m_problem) {
    case Problem::None: return {};
    case Problem::Missing: return tr("%1 isn't there any more.").arg(name);
    case Problem::Unreadable: return tr("%1 couldn't be read. It may be damaged.").arg(name);
    case Problem::Unsupported:
        return tr("Omaread can't read %1. It opens PDF, EPUB, comic archives, Markdown and text files.").arg(name);
    case Problem::Empty: return tr("%1 has nothing in it to read.").arg(name);
    case Problem::Picker: return m_problemDetail;
    }
    return {};
}

void Backend::setProblem(Problem problem, const QString &detail) {
    if (m_problem == problem && m_problemDetail == detail)
        return;
    m_problem = problem;
    m_problemDetail = detail;
    emit errorChanged();
}

void Backend::dismissError() {
    setProblem(Problem::None);
}

QString Backend::passwordFor() const {
    return m_passwordPath.isEmpty() ? QString() : QFileInfo(m_passwordPath).fileName();
}

void Backend::unlock(const QString &password) {
    if (!m_passwordPath.isEmpty())
        openFile(m_passwordPath, password);
}

void Backend::cancelPassword() {
    if (m_passwordPath.isEmpty())
        return;
    m_passwordPath.clear();
    m_passwordWrong = false;
    emit passwordChanged();
}

QVariantList Backend::toc() const {
    QVariantList list;
    if (m_book)
        for (const TocEntry &entry : m_book->toc)
            list << QVariantMap{{"title", entry.title}, {"section", entry.section}, {"depth", entry.depth}};
    return list;
}

QVariantList Backend::aspects() const {
    QVariantList list;
    if (m_book)
        for (const double aspect : m_book->aspects)
            list << aspect;
    return list;
}

int Backend::textFormat() const {
    if (!m_book)
        return Qt::PlainText;
    switch (m_book->kind) {
    case Book::Kind::Epub: return Qt::RichText;
    case Book::Kind::Markdown: return Qt::MarkdownText;
    default: return Qt::PlainText;
    }
}

QUrl Backend::baseUrl() const {
    return m_book ? QUrl::fromLocalFile(QFileInfo(m_book->path).absolutePath() + '/') : QUrl();
}

int Backend::tocIndex() const {
    int found = -1;
    if (m_book)
        for (int i = 0; i < m_book->toc.size(); ++i)
            if (m_book->toc[i].section <= m_section)
                found = i;
    return found;
}

double Backend::progress() const {
    const int count = sectionCount();
    if (count == 0)
        return 0;
    if (paged()) {
        const int last = m_spread ? std::min(m_section + 2, count) : m_section + 1;
        return std::clamp(double(last) / count, 0.0, 1.0);
    }
    return std::clamp((m_section + m_position) / count, 0.0, 1.0);
}

void Backend::setPosition(double position) {
    position = std::clamp(position, 0.0, 1.0);
    if (qFuzzyCompare(1 + position, 1 + m_position))
        return;
    m_position = position;
    emit positionChanged();
    remember();
}

void Backend::setTarget(int target) {
    if (target == m_target)
        return;
    m_target = target;
    emit targetChanged();
}

void Backend::setReadingWidth(int width) {
    if (width == m_readingWidth)
        return;
    m_readingWidth = width;
    emit readingWidthChanged();
    updateText();
}

void Backend::setTextScale(double scale) {
    scale = std::clamp(scale, 0.8, 4.0);
    if (qFuzzyCompare(scale, m_textScale))
        return;
    m_textScale = scale;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setTintPages(bool tint) {
    if (tint == m_tintPages)
        return;
    m_tintPages = tint;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setSpread(bool spread) {
    if (spread == m_spread)
        return;
    m_spread = spread;
    emit settingsChanged();
    updateLinks();
    emit sectionChanged();
    emit hitChanged();
    emit positionChanged();
    scheduleSave();
}

void Backend::updateText() {
    QString text;
    if (m_book && !m_book->paged() && m_section < m_book->sections.size()) {
        text = m_book->sections[m_section];
        if (m_book->kind == Book::Kind::Epub)
            text = Book::fitImages(text, m_readingWidth);
    }
    if (text != m_text) {
        m_text = text;
        emit textChanged();
    }
}

void Backend::updateLinks() {
    m_links.clear();
    if (!m_book || m_book->kind != Book::Kind::Pdf)
        return;
    const int last = m_spread ? std::min(m_section + 1, sectionCount() - 1) : m_section;
    for (int page = m_section; page <= last; ++page) {
        for (const PageLink &link : m_book->pageLinks(page)) {
            QVariantMap map = area(page, link.rect);
            map.insert("target", link.page);
            map.insert("url", link.url);
            m_links << map;
        }
    }
}

void Backend::openUrl(const QUrl &url) {
    if (!url.isLocalFile()) {
        setProblem(Problem::Unsupported, url.fileName());
        return;
    }
    openFile(url.toLocalFile());
}

void Backend::openFile(const QString &path, const QString &password) {
    const int generation = ++m_generation;
    const int id = m_nextId++;
    if (!m_loading) {
        m_loading = true;
        emit loadingChanged();
    }
    // A large book takes a moment to unpack; the window stays responsive.
    auto *watcher = new QFutureWatcher<Loaded>(this);
    connect(watcher, &QFutureWatcher<Loaded>::finished, this, [this, watcher, generation, path] {
        loaded(generation, path, watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, id, password] {
        Loaded result;
        result.book = Book::load(path, id, &result.error, password);
        return result;
    }));
}

void Backend::loaded(int generation, const QString &path, const Loaded &result) {
    // Only the most recent request counts; an earlier one finishing late is dropped.
    if (generation != m_generation)
        return;
    m_loading = false;
    emit loadingChanged();
    if (!result.book) {
        if (result.error == Book::Error::Locked || result.error == Book::Error::WrongPassword) {
            // Asked for in the window; never kept, so a locked file asks each time.
            m_passwordPath = path;
            m_passwordWrong = result.error == Book::Error::WrongPassword;
            setProblem(Problem::None);
            emit passwordChanged();
            return;
        }
        switch (result.error) {
        case Book::Error::Missing: setProblem(Problem::Missing, path); break;
        case Book::Error::Unsupported: setProblem(Problem::Unsupported, path); break;
        case Book::Error::Empty: setProblem(Problem::Empty, path); break;
        default: setProblem(Problem::Unreadable, path); break;
        }
        return;
    }
    setProblem(Problem::None);
    cancelPassword();
    clearSearch();
    if (m_saveTimer.isActive())
        save();

    m_book = result.book;
    m_open->set(m_book);
    m_section = 0;
    m_position = 0;
    const auto it = std::find_if(m_library.begin(), m_library.end(),
                                 [&](const Entry &entry) { return entry.path == m_book->path; });
    if (it != m_library.end()) {
        m_section = std::clamp(it->section, 0, m_book->sectionCount() - 1);
        m_position = it->position;
        m_library.erase(it);
    }
    m_library.prepend(Entry{m_book->path, m_book->title, m_book->author, m_book->kindName(), m_section,
                            m_book->sectionCount(), m_position, QDateTime::currentMSecsSinceEpoch()});
    while (m_library.size() > maxLibrary)
        m_library.removeLast();
    scheduleSave();

    setTarget(-1);
    updateLinks();
    emit bookChanged();
    updateText();
    emit sectionChanged();
    emit positionChanged();
    emit libraryChanged();
}

void Backend::openRecent(int index) {
    if (index >= 0 && index < m_library.size())
        openFile(m_library[index].path);
}

void Backend::forget(int index) {
    if (index < 0 || index >= m_library.size())
        return;
    m_library.removeAt(index);
    emit libraryChanged();
    scheduleSave();
}

void Backend::pickFile() {
    m_picker->openDocument();
}

void Backend::close() {
    if (!m_book)
        return;
    save();
    clearSearch();
    m_book.reset();
    m_open->set(nullptr);
    m_section = 0;
    m_position = 0;
    setTarget(-1);
    m_links.clear();
    emit bookChanged();
    updateText();
    emit sectionChanged();
    emit positionChanged();
    emit libraryChanged();
}

void Backend::goTo(int section, double position) {
    if (!m_book)
        return;
    section = std::clamp(section, 0, m_book->sectionCount() - 1);
    position = std::clamp(position, 0.0, 1.0);
    const bool moved = section != m_section;
    m_section = section;
    m_position = position;
    setTarget(-1);
    if (moved) {
        updateText();
        updateLinks();
        emit sectionChanged();
        emit hitChanged();
    }
    emit positionChanged();
    remember();
}

void Backend::goToEntry(int index) {
    if (!m_book || index < 0 || index >= m_book->toc.size())
        return;
    const TocEntry &entry = m_book->toc[index];
    goTo(entry.section, 0);
    setTarget(m_book->fragmentOffset(entry.section, entry.fragment));
}

bool Backend::followLink(const QString &link) {
    if (!m_book)
        return false;
    QString fragment;
    const int section = m_book->sectionForLink(link, &fragment);
    if (section < 0)
        return false;
    goTo(section, 0);
    setTarget(m_book->fragmentOffset(section, fragment));
    return true;
}

void Backend::remember() {
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return;
    m_library.first().section = m_section;
    m_library.first().position = m_position;
    scheduleSave();
}

void Backend::find(const QString &words) {
    const QString query = words.simplified();
    if (!m_book || query.isEmpty()) {
        clearSearch();
        return;
    }
    if (m_searchCancel)
        *m_searchCancel = true;
    m_searchCancel = std::make_shared<std::atomic_bool>(false);
    const int generation = ++m_searchGeneration;
    m_query = query;
    m_searching = true;
    m_hits.clear();
    m_hitIndex = -1;
    emit searchChanged();
    emit hitChanged();

    // A long book takes a while to search, so it runs beside the window.
    auto *watcher = new QFutureWatcher<QList<SearchHit>>(this);
    connect(watcher, &QFutureWatcher<QList<SearchHit>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_searchGeneration)
            return;
        m_hits = watcher->result();
        m_searching = false;
        emit searchChanged();
        // The first match from the reader's place on, or else from the start.
        int first = 0;
        for (int i = 0; i < m_hits.size(); ++i) {
            if (m_hits[i].section >= m_section) {
                first = i;
                break;
            }
        }
        showHit(m_hits.isEmpty() ? -1 : first);
    });
    watcher->setFuture(QtConcurrent::run([book = m_book, query, cancel = m_searchCancel] {
        return book->search(query, cancel.get());
    }));
}

void Backend::showHit(int index) {
    m_hitIndex = index;
    if (index >= 0 && m_hits[index].section != m_section) {
        // A spread already showing the match's page stays as it is.
        const bool shown = m_book->paged() && m_spread && m_hits[index].section == m_section + 1;
        if (!shown)
            goTo(m_hits[index].section, 0);
    }
    emit hitChanged();
}

void Backend::nextHit() {
    if (!m_hits.isEmpty())
        showHit((m_hitIndex + 1) % int(m_hits.size()));
}

void Backend::previousHit() {
    if (!m_hits.isEmpty())
        showHit((m_hitIndex - 1 + int(m_hits.size())) % int(m_hits.size()));
}

void Backend::clearSearch() {
    if (m_searchCancel)
        *m_searchCancel = true;
    ++m_searchGeneration;
    if (m_query.isEmpty() && m_hits.isEmpty() && !m_searching)
        return;
    m_query.clear();
    m_searching = false;
    m_hits.clear();
    m_hitIndex = -1;
    emit searchChanged();
    emit hitChanged();
}

QVariantList Backend::pageHits() const {
    QVariantList list;
    if (!paged())
        return list;
    const int last = m_spread ? m_section + 1 : m_section;
    for (int i = 0; i < m_hits.size(); ++i) {
        const SearchHit &hit = m_hits[i];
        if (hit.section < m_section || hit.section > last)
            continue;
        QVariantMap map = area(hit.section, hit.rect);
        map.insert("current", i == m_hitIndex);
        list << map;
    }
    return list;
}

bool Backend::copyPageText(int page, double x, double y, double width, double height) {
    if (!m_book)
        return false;
    const QString text = m_book->pageText(page, QRectF(x, y, width, height));
    if (text.isEmpty())
        return false;
    copyText(text);
    return true;
}

void Backend::copyText(const QString &text) {
    if (auto *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

void Backend::spaceLines(QQuickTextDocument *document, double factor) {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text || text->isEmpty())
        return;
    const qreal height = factor * 100;
    // Already spaced: changing it again would only lay the chapter out twice.
    if (qFuzzyCompare(text->firstBlock().blockFormat().lineHeight(), height)
        && text->firstBlock().blockFormat().lineHeightType() == QTextBlockFormat::ProportionalHeight)
        return;
    QTextBlockFormat format;
    format.setLineHeight(height, QTextBlockFormat::ProportionalHeight);
    QTextCursor cursor(text);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(format);
}
