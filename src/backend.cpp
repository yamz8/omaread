#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
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

QString kindName(Book::Kind kind) {
    switch (kind) {
    case Book::Kind::Pdf: return "pdf";
    case Book::Kind::Epub: return "epub";
    case Book::Kind::Comic: return "comic";
    case Book::Kind::Markdown: return "markdown";
    case Book::Kind::Text: return "text";
    }
    return "text";
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
    if (m_saveTimer.isActive())
        save();
}

void Backend::readState() {
    QFile file(m_stateFile);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
    m_textSize = std::clamp(state.value("textSize").toInt(m_textSize), 12, 48);
    m_tintPages = state.value("tintPages").toBool();
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
    const QJsonObject state{{"textSize", m_textSize}, {"tintPages", m_tintPages}, {"books", books}};
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
    case Problem::Locked: return tr("%1 is locked with a password.").arg(name);
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

QString Backend::kind() const {
    return m_book ? kindName(m_book->kind) : QString();
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
    return std::clamp(paged() ? double(m_section + 1) / count : (m_section + m_position) / count, 0.0, 1.0);
}

void Backend::setPosition(double position) {
    position = std::clamp(position, 0.0, 1.0);
    if (qFuzzyCompare(1 + position, 1 + m_position))
        return;
    m_position = position;
    emit positionChanged();
    remember();
}

void Backend::setReadingWidth(int width) {
    if (width == m_readingWidth)
        return;
    m_readingWidth = width;
    emit readingWidthChanged();
    updateText();
}

void Backend::setTextSize(int size) {
    size = std::clamp(size, 12, 48);
    if (size == m_textSize)
        return;
    m_textSize = size;
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

void Backend::openUrl(const QUrl &url) {
    if (!url.isLocalFile()) {
        setProblem(Problem::Unsupported, url.fileName());
        return;
    }
    openFile(url.toLocalFile());
}

void Backend::openFile(const QString &path) {
    const int generation = ++m_generation;
    const int id = m_nextId++;
    if (!m_loading) {
        m_loading = true;
        emit loadingChanged();
    }
    // A large book takes a moment to unpack; the window stays responsive.
    auto *watcher = new QFutureWatcher<Loaded>(this);
    connect(watcher, &QFutureWatcher<Loaded>::finished, this, [this, watcher, generation] {
        loaded(generation, watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, id] {
        Loaded result;
        result.book = Book::load(path, id, &result.error);
        return result;
    }));
    m_pendingPath = path;
}

void Backend::loaded(int generation, const Loaded &result) {
    // Only the most recent request counts; an earlier one finishing late is dropped.
    if (generation != m_generation)
        return;
    m_loading = false;
    emit loadingChanged();
    if (!result.book) {
        const QString path = m_pendingPath;
        switch (result.error) {
        case Book::Error::Missing: setProblem(Problem::Missing, path); break;
        case Book::Error::Unsupported: setProblem(Problem::Unsupported, path); break;
        case Book::Error::Locked: setProblem(Problem::Locked, path); break;
        case Book::Error::Empty: setProblem(Problem::Empty, path); break;
        default: setProblem(Problem::Unreadable, path); break;
        }
        return;
    }
    setProblem(Problem::None);
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
    m_library.prepend(Entry{m_book->path, m_book->title, m_book->author, kindName(m_book->kind), m_section,
                            m_book->sectionCount(), m_position, QDateTime::currentMSecsSinceEpoch()});
    while (m_library.size() > maxLibrary)
        m_library.removeLast();
    scheduleSave();

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
    m_book.reset();
    m_open->set(nullptr);
    m_section = 0;
    m_position = 0;
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
    if (moved) {
        updateText();
        emit sectionChanged();
    }
    emit positionChanged();
    remember();
}

bool Backend::followLink(const QString &link) {
    if (!m_book)
        return false;
    const int section = m_book->sectionForLink(link);
    if (section < 0)
        return false;
    goTo(section, 0);
    return true;
}

void Backend::remember() {
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return;
    m_library.first().section = m_section;
    m_library.first().position = m_position;
    scheduleSave();
}
