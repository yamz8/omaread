#include "backend.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQuickTextDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QThreadPool>
#include <QtConcurrent>

#include <algorithm>
#include <tuple>
#include <cmath>

#include "layout.h"
#include "portalfilepicker.h"
#include "stylesheet.h"

namespace {

// Enough to find any recent book again without the list becoming an archive.
constexpr int maxOpened = 500;
// Covers are kept this tall: enough for the library's grid on any screen.
constexpr int coverHeight = 480;
// The files a watched folder's books are.
const QStringList bookSuffixes = {"epub", "pdf", "cbz", "cbr", "cb7", "cbt", "md", "markdown", "txt"};

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
    m_coverDirectory = stateDirectory.isEmpty()
        ? (qEnvironmentVariable("XDG_CACHE_HOME").isEmpty() ? QDir::homePath() + "/.cache" : qEnvironmentVariable("XDG_CACHE_HOME"))
              + "/omaread/covers"
        : stateDirectory + "/covers";
    // Books put in ~/Books join the library by themselves; tests choose their own.
    if (stateDirectory.isEmpty())
        m_folders = {QDir::homePath() + "/Books"};
    m_scanTimer.setSingleShot(true);
    m_scanTimer.setInterval(400);
    connect(&m_scanTimer, &QTimer::timeout, this, &Backend::scanFolders);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_scanTimer, qOverload<>(&QTimer::start));
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(800);
    connect(&m_saveTimer, &QTimer::timeout, this, &Backend::save);
    m_pageTimer.setSingleShot(true);
    m_pageTimer.setInterval(250);
    connect(&m_pageTimer, &QTimer::timeout, this, &Backend::countPages);
    connect(m_picker, &FilePicker::openSelected, this, &Backend::openUrl);
    connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setProblem(Problem::Picker, message); });
    readState();
    // After the window has what it needs: the folders, then the covers.
    QTimer::singleShot(0, this, [this] {
        scanFolders();
        fillCovers();
    });
}

Backend::~Backend() {
    if (m_searchCancel)
        *m_searchCancel = true;
    if (m_pageCancel)
        *m_pageCancel = true;
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
    if (const QString transition = state.value("transition").toString(); transitions().contains(transition))
        m_transition = transition;
    if (const QString value = state.value("typeface").toString(); typefaces().contains(value))
        m_typeface = value;
    if (const QString value = state.value("lineSpacing").toString(); lineSpacings().contains(value))
        m_lineSpacing = value;
    if (const QString value = state.value("columnWidth").toString(); columnWidths().contains(value))
        m_columnWidth = value;
    m_justify = state.value("justify").toBool();
    m_sidebar = state.value("sidebar").toBool();
    m_trimMargins = state.value("trimMargins").toBool();
    const QJsonObject days = state.value("readingDays").toObject();
    for (auto it = days.begin(); it != days.end(); ++it)
        m_readingDays.insert(it.key(), it.value().toDouble());
    m_wordsPerMinute = std::clamp(state.value("wordsPerMinute").toDouble(defaultWordsPerMinute), 80.0, 900.0);
    if (const QString value = state.value("progressShows").toString(); progressModes().contains(value))
        m_progressShows = value;
    if (const QString value = state.value("libraryView").toString(); value == "grid" || value == "list")
        m_libraryView = value;
    if (const QString value = state.value("librarySort").toString(); QStringList{"recent", "title", "author", "progress"}.contains(value))
        m_librarySort = value;
    if (const QString value = state.value("libraryFilter").toString(); QStringList{"all", "reading", "unread", "finished"}.contains(value))
        m_libraryFilter = value;
    if (state.contains("folders")) {
        m_folders.clear();
        for (const QJsonValue &folder : state.value("folders").toArray())
            m_folders << folder.toString();
    }
    for (const QJsonValue &value : state.value("books").toArray()) {
        const QJsonObject book = value.toObject();
        Entry entry{book.value("path").toString(), book.value("title").toString(), book.value("author").toString(),
                    book.value("kind").toString(), book.value("section").toInt(), book.value("sections").toInt(),
                    std::clamp(book.value("position").toDouble(), 0.0, 1.0), qint64(book.value("opened").toDouble()),
                    book.value("progress").toDouble(-1), {}, {},
                    book.value("finished").toBool(), qint64(book.value("finishedAt").toDouble()),
                    book.value("rightToLeft").toInt(-1),
                    qint64(book.value("added").toDouble()), book.value("hidden").toBool()};
        for (const QJsonValue &mark : book.value("highlights").toArray()) {
            const QJsonObject m = mark.toObject();
            Highlight h{m.value("section").toInt(), m.value("start").toInt(), m.value("end").toInt(),
                        m.value("text").toString(), m.value("note").toString(), m.value("color").toString(), {}};
            if (!highlightColors().contains(h.color))
                h.color = "yellow";
            if (const QJsonArray area = m.value("area").toArray(); area.size() == 4)
                h.area = QRectF(area[0].toDouble(), area[1].toDouble(), area[2].toDouble(), area[3].toDouble());
            if (h.end > h.start || !h.area.isEmpty())
                entry.highlights << h;
        }
        for (const QJsonValue &mark : book.value("bookmarks").toArray()) {
            const QJsonObject m = mark.toObject();
            entry.bookmarks << Entry::Bookmark{m.value("section").toInt(),
                                               std::clamp(m.value("position").toDouble(), 0.0, 1.0),
                                               m.value("text").toString()};
        }
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
                                 {"position", entry.position}, {"opened", double(entry.opened)},
                                 {"progress", entry.progress}, {"finished", entry.finished},
                                 {"added", double(entry.added)}, {"hidden", entry.hidden},
                                 {"rightToLeft", entry.rightToLeft}, {"finishedAt", double(entry.finishedAt)},
                                 {"highlights", [&] {
                                      QJsonArray marks;
                                      for (const Highlight &h : entry.highlights)
                                      {
                                          QJsonObject mark{{"section", h.section}, {"start", h.start},
                                                           {"end", h.end},         {"text", h.text},
                                                           {"note", h.note},       {"color", h.color}};
                                          if (!h.area.isEmpty())
                                              mark.insert("area", QJsonArray{h.area.x(), h.area.y(), h.area.width(),
                                                                             h.area.height()});
                                          marks.append(mark);
                                      }
                                      return marks;
                                  }()},
                                 {"bookmarks", [&] {
                                      QJsonArray marks;
                                      for (const Entry::Bookmark &mark : entry.bookmarks)
                                          marks.append(QJsonObject{{"section", mark.section},
                                                                   {"position", mark.position},
                                                                   {"text", mark.text}});
                                      return marks;
                                  }()}});
    const QJsonObject state{
        {"textScale", m_textScale}, {"tintPages", m_tintPages}, {"spread", m_spread}, {"transition", m_transition},
        {"typeface", m_typeface},   {"lineSpacing", m_lineSpacing}, {"columnWidth", m_columnWidth},
        {"justify", m_justify}, {"sidebar", m_sidebar},     {"wordsPerMinute", m_wordsPerMinute},
        {"progressShows", m_progressShows}, {"folders", QJsonArray::fromStringList(m_folders)},
        {"trimMargins", m_trimMargins}, {"libraryView", m_libraryView}, {"readingDays", [&] {
             QJsonObject days;
             for (auto it = m_readingDays.cbegin(); it != m_readingDays.cend(); ++it)
                 days.insert(it.key(), std::round(it.value()));
             return days;
         }()}, {"librarySort", m_librarySort}, {"libraryFilter", m_libraryFilter},
        {"books", books}};
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
        if (entry.hidden)
            continue;
        const double progress =
            entry.progress >= 0 ? entry.progress
            : entry.sections <= 0 ? 0
            : entry.kind == "pdf" || entry.kind == "comic"
                ? double(entry.section + 1) / entry.sections
                : (entry.section + entry.position) / entry.sections;
        list << QVariantMap{{"path", entry.path},
                            // A title that is only the file's name, as older entries kept it, tidied.
                            {"title", entry.title.isEmpty() || entry.title == QFileInfo(entry.path).completeBaseName()
                                          ? Book::titleFromFileName(entry.path) : entry.title},
                            {"author", entry.author},
                            {"kind", entry.kind},
                            {"fileName", QFileInfo(entry.path).fileName()},
                            {"progress", entry.finished ? 1.0 : std::clamp(progress, 0.0, 1.0)},
                            {"exists", QFileInfo::exists(entry.path)},
                            {"finished", entry.finished},
                            {"started", entry.opened > 0},
                            // Most recently opened, or for one never opened, arrived.
                            {"recent", double(entry.opened > 0 ? entry.opened : entry.added)},
                            {"cover", QFileInfo::exists(coverFile(entry.path))
                                          ? QUrl::fromLocalFile(coverFile(entry.path)).toString() : QString()}};
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
    // By words, and only of the book's text: a title page weighs nothing and
    // a long chapter more than a short one.
    const qint64 total = m_book->totalWords();
    if (total <= 0)
        return std::clamp((m_section + m_position) / count, 0.0, 1.0);
    return std::clamp(double(m_book->wordsBefore(m_section, m_position)) / total, 0.0, 1.0);
}

QString Backend::chapterLabel() const {
    return chapterLabelFor(m_section);
}

QString Backend::chapterLabelFor(int section) const {
    return m_book ? m_book->chapterLabel(section) : QString();
}

int Backend::minutesLeftInChapter() const {
    if (!m_book || m_book->words.isEmpty() || m_section >= m_book->words.size())
        return -1;
    const double left = m_book->words[m_section] * (1 - m_position);
    return int(std::ceil(left / m_wordsPerMinute));
}

int Backend::minutesLeftInBook() const {
    if (!m_book || m_book->words.isEmpty() || m_book->totalWords() <= 0)
        return -1;
    const double left = double(m_book->totalWords() - m_book->wordsBefore(m_section, m_position));
    return int(std::ceil(std::max(0.0, left) / m_wordsPerMinute));
}

void Backend::noteReading(double seconds, double fraction) {
    if (!m_book || m_book->words.isEmpty() || m_section >= m_book->words.size() || seconds <= 0)
        return;
    const double words = m_book->words[m_section] * std::clamp(fraction, 0.0, 1.0);
    // A screen too short to have been read, or one left open while away,
    // says nothing about the reader's pace.
    if (words < 40 || seconds < 4 || seconds > 300)
        return;
    const double pace = words / seconds * 60;
    if (pace < 60 || pace > 1200)
        return;
    // Moves a tenth of the way each screen, so one slow page doesn't swing it.
    m_wordsPerMinute = std::clamp(m_wordsPerMinute * 0.9 + pace * 0.1, 80.0, 900.0);
    emit positionChanged();
    scheduleSave();
}

void Backend::noteTimeRead(double seconds) {
    if (!m_book || seconds < 2)
        return;
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    m_readingDays[today] += std::min(seconds, 300.0);
    // A year and a little is all the counts ask for.
    while (m_readingDays.size() > 400)
        m_readingDays.erase(m_readingDays.begin());
    emit statsChanged();
    scheduleSave();
}

QVariantMap Backend::readingStats() const {
    const QDate today = QDate::currentDate();
    double week = 0;
    for (int day = 0; day < 7; ++day)
        week += m_readingDays.value(today.addDays(-day).toString(Qt::ISODate));
    int finished = 0;
    for (const Entry &entry : m_library)
        if (entry.finished && entry.finishedAt > 0
            && QDateTime::fromMSecsSinceEpoch(entry.finishedAt).date().year() == today.year())
            ++finished;
    return {{"today", int(m_readingDays.value(today.toString(Qt::ISODate)) / 60)},
            {"week", int(week / 60)},
            {"finishedThisYear", finished}};
}

void Backend::setProgressShows(const QString &shows) {
    if (shows == m_progressShows || !progressModes().contains(shows))
        return;
    m_progressShows = shows;
    emit settingsChanged();
    scheduleSave();
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

void Backend::setReadingSize(int size) {
    if (size == m_readingSize || size <= 0)
        return;
    m_readingSize = size;
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

void Backend::setTransition(const QString &transition) {
    if (transition == m_transition || !transitions().contains(transition))
        return;
    m_transition = transition;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setChoice(QString *setting, const QString &value, const QStringList &choices) {
    if (value == *setting || !choices.contains(value))
        return;
    *setting = value;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setTypeface(const QString &typeface) {
    if (typeface == m_typeface || !typefaces().contains(typeface))
        return;
    m_typeface = typeface;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setLineSpacing(const QString &spacing) {
    if (spacing == m_lineSpacing || !lineSpacings().contains(spacing))
        return;
    m_lineSpacing = spacing;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setColumnWidth(const QString &width) {
    if (width == m_columnWidth || !columnWidths().contains(width))
        return;
    m_columnWidth = width;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setJustify(bool justify) {
    if (justify == m_justify)
        return;
    m_justify = justify;
    emit settingsChanged();
    scheduleSave();
}

void Backend::setSidebar(bool sidebar) {
    if (sidebar == m_sidebar)
        return;
    m_sidebar = sidebar;
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

void Backend::setInk(const QColor &ink) {
    if (ink == m_ink)
        return;
    m_ink = ink;
    updateText();
}

void Backend::updateText() {
    QString text;
    if (m_book && !m_book->paged() && m_section < m_book->sections.size()) {
        text = m_book->sections[m_section];
        if (m_book->kind == Book::Kind::Epub)
            text = Styling::sized(Book::withInk(Book::fitImages(text, m_readingWidth), m_ink), m_readingSize);
    }
    if (text != m_text) {
        emit textAboutToChange();
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
    QList<Entry::Bookmark> bookmarks;
    QList<Highlight> highlights;
    bool finished = false;
    qint64 finishedAt = 0;
    int rightToLeft = -1;
    qint64 added = 0;
    const auto it = std::find_if(m_library.begin(), m_library.end(),
                                 [&](const Entry &entry) { return entry.path == m_book->path; });
    if (it != m_library.end()) {
        m_section = std::clamp(it->section, 0, m_book->sectionCount() - 1);
        m_position = it->position;
        bookmarks = it->bookmarks;
        highlights = it->highlights;
        finished = it->finished;
        finishedAt = it->finishedAt;
        rightToLeft = it->rightToLeft;
        added = it->added;
        m_library.erase(it);
    }
    m_library.prepend(Entry{m_book->path, m_book->title, m_book->author, m_book->kindName(), m_section,
                            m_book->sectionCount(), m_position, QDateTime::currentMSecsSinceEpoch(), progress(),
                            bookmarks, highlights, finished, finishedAt, rightToLeft, added, false});
    // Enough to find any recent book again without the list becoming an
    // archive; books in a watched folder stay whatever their age.
    int opened = 0;
    for (qsizetype i = 0; i < m_library.size(); ++i)
        if (m_library[i].opened > 0 && ++opened > maxOpened && m_library[i].added == 0)
            m_library.removeAt(i--);
    saveCover(m_book);
    // Ready for a search before one is asked for.
    QThreadPool::globalInstance()->start([book = m_book] { book->preparePlainText(); }, -1);
    scheduleSave();

    setTarget(-1);
    updateLinks();
    m_back.clear();
    m_forward.clear();
    emit historyChanged();
    emit bookmarksChanged();
    emit highlightsChanged();
    m_panels.clear();
    m_panelsAsked.clear();
    m_contentBoxes.clear();
    emit bookChanged();
    updateText();
    emit sectionChanged();
    emit positionChanged();
    emit libraryChanged();
    countPages();
}

void Backend::openRecent(int index) {
    if (index >= 0 && index < m_library.size())
        openFile(m_library[index].path);
}

void Backend::forgetPath(const QString &path) {
    const auto it = std::find_if(m_library.begin(), m_library.end(), [&](const Entry &e) { return e.path == path; });
    if (it == m_library.end())
        return;
    // A book in a watched folder would come straight back: it is hidden instead.
    const bool watched = std::any_of(m_folders.begin(), m_folders.end(), [&](const QString &folder) {
        return path.startsWith(QDir::cleanPath(folder) + '/');
    });
    if (watched && QFileInfo::exists(path))
        *it = Entry{path, it->title, it->author, it->kind, 0, it->sections, 0, 0, -1, {}, {}, false, 0, -1, it->added, true};
    else
        m_library.erase(it);
    emit libraryChanged();
    scheduleSave();
}

void Backend::setFinished(const QString &path, bool finished) {
    for (Entry &entry : m_library)
        if (entry.path == path && entry.finished != finished) {
            entry.finished = finished;
            entry.finishedAt = finished ? QDateTime::currentMSecsSinceEpoch() : 0;
            emit statsChanged();
            emit libraryChanged();
            scheduleSave();
        }
}

QString Backend::coverFile(const QString &path) const {
    const QFileInfo info(path);
    // A changed file is a new cover.
    const QByteArray key = path.toUtf8() + '|' + QByteArray::number(info.size()) + '|'
                           + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    return m_coverDirectory + '/' + QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex() + ".jpg";
}

void Backend::saveCover(const std::shared_ptr<Book> &book) {
    const QString file = coverFile(book->path);
    if (QFileInfo::exists(file))
        return;
    m_coverTried.insert(book->path);
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
        watcher->deleteLater();
        if (watcher->result())
            emit libraryChanged();
    });
    const QString directory = m_coverDirectory;
    watcher->setFuture(QtConcurrent::run([book, file, directory] {
        const QImage cover = book->cover(coverHeight);
        if (cover.isNull())
            return false;
        QDir().mkpath(directory);
        QSaveFile out(file);
        out.setDirectWriteFallback(false);
        return out.open(QIODevice::WriteOnly) && cover.convertToFormat(QImage::Format_RGB32).save(&out, "JPG", 85)
               && out.commit();
    }));
}

// One book at a time, in the background: a book listed without a cover is
// read for it, and for its title and author when it was never opened.
void Backend::fillCovers() {
    if (m_coverBusy)
        return;
    const auto it = std::find_if(m_library.begin(), m_library.end(), [&](const Entry &entry) {
        return !entry.hidden && !m_coverTried.contains(entry.path) && QFileInfo::exists(entry.path)
               && !QFileInfo::exists(coverFile(entry.path));
    });
    if (it == m_library.end())
        return;
    const QString path = it->path, file = coverFile(path), directory = m_coverDirectory;
    m_coverTried.insert(path);
    m_coverBusy = true;
    struct Found {
        QString title, author, kind;
        int sections = 0;
        bool cover = false;
    };
    auto *watcher = new QFutureWatcher<Found>(this);
    connect(watcher, &QFutureWatcher<Found>::finished, this, [this, watcher, path] {
        watcher->deleteLater();
        m_coverBusy = false;
        const Found found = watcher->result();
        for (Entry &entry : m_library)
            if (entry.path == path && entry.opened == 0 && !found.title.isEmpty()) {
                entry.title = found.title;
                entry.author = found.author;
                entry.kind = found.kind;
                entry.sections = found.sections;
                scheduleSave();
            }
        emit libraryChanged();
        fillCovers();
    });
    watcher->setFuture(QtConcurrent::run([path, file, directory] {
        Found found;
        Book::Error error = Book::Error::None;
        const auto book = Book::load(path, 0, &error);
        if (!book)
            return found;
        found = {book->title, book->author, book->kindName(), book->sectionCount(), false};
        const QImage cover = book->cover(coverHeight);
        if (!cover.isNull()) {
            QDir().mkpath(directory);
            QSaveFile out(file);
            out.setDirectWriteFallback(false);
            found.cover = out.open(QIODevice::WriteOnly)
                          && cover.convertToFormat(QImage::Format_RGB32).save(&out, "JPG", 85) && out.commit();
        }
        return found;
    }));
}

void Backend::setFolders(const QStringList &folders) {
    m_folders = folders;
    scanFolders();
    scheduleSave();
}

// The books in the watched folders: a new one joins the library, not yet
// started, and one gone from there that was never opened leaves it.
void Backend::scanFolders() {
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    QSet<QString> found;
    QStringList watched;
    for (const QString &folder : std::as_const(m_folders)) {
        if (!QFileInfo(folder).isDir())
            continue;
        watched << folder;
        QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                        QDirIterator::Subdirectories);
        while (it.hasNext() && found.size() < 5000) {
            const QFileInfo info = it.nextFileInfo();
            if (info.isDir()) {
                if (!info.fileName().startsWith('.'))
                    watched << info.absoluteFilePath();
                continue;
            }
            if (info.fileName().startsWith('.') || !bookSuffixes.contains(info.suffix().toLower()))
                continue;
            found.insert(info.absoluteFilePath());
        }
    }
    if (!watched.isEmpty())
        m_watcher.addPaths(watched);
    bool changed = false;
    QSet<QString> listed;
    for (const Entry &entry : std::as_const(m_library))
        listed.insert(entry.path);
    for (const QString &path : std::as_const(found)) {
        if (listed.contains(path))
            continue;
        const QFileInfo info(path);
        const QString suffix = info.suffix().toLower();
        const QString kind = suffix == "pdf" ? "pdf" : suffix == "epub" ? "epub"
                           : suffix.startsWith("cb") ? "comic" : suffix == "txt" ? "text" : "markdown";
        m_library << Entry{path, Book::titleFromFileName(path), {}, kind, 0, 0, 0, 0, -1, {}, {}, false, 0, -1,
                           info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch()
                                                      : info.lastModified().toMSecsSinceEpoch(), false};
        changed = true;
    }
    const qsizetype before = m_library.size();
    m_library.removeIf([&](const Entry &entry) {
        const bool inFolder = std::any_of(m_folders.begin(), m_folders.end(), [&](const QString &folder) {
            return entry.path.startsWith(QDir::cleanPath(folder) + '/');
        });
        return inFolder && entry.opened == 0 && !found.contains(entry.path);
    });
    changed = changed || m_library.size() != before;
    if (changed) {
        emit libraryChanged();
        scheduleSave();
        fillCovers();
    }
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
    m_back.clear();
    m_forward.clear();
    emit historyChanged();
    emit bookmarksChanged();
    emit highlightsChanged();
    m_panels.clear();
    m_panelsAsked.clear();
    m_contentBoxes.clear();
    emit bookChanged();
    updateText();
    emit sectionChanged();
    emit positionChanged();
    emit libraryChanged();
    countPages();
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
        // The window hears of the new section before its text arrives, so the
        // old chapter's scroll, as the new text is laid out, isn't taken for
        // the reader's place in the new one.
        updateLinks();
        emit sectionChanged();
        updateText();
        emit hitChanged();
    }
    emit positionChanged();
    remember();
}

void Backend::goToEntry(int index) {
    if (!m_book || index < 0 || index >= m_book->toc.size())
        return;
    const TocEntry &entry = m_book->toc[index];
    markPlace();
    goTo(entry.section, 0);
    setTarget(m_book->fragmentOffset(entry.section, entry.fragment));
}

QString Backend::note(const QString &link) const {
    if (!m_book || m_book->notes.isEmpty())
        return {};
    QString fragment;
    const int section = m_book->sectionForLink(link, &fragment);
    if (section < 0 || fragment.isEmpty())
        return {};
    const QString html = m_book->notes.value(QString("%1#%2").arg(section).arg(fragment));
    return html.isEmpty() ? QString() : Styling::sized(Book::withInk(html, m_ink), m_readingSize);
}

// The open book's entry in the library, which keeps its bookmarks.
Backend::Entry *Backend::openEntry() {
    return m_book && !m_library.isEmpty() && m_library.first().path == m_book->path ? &m_library.first() : nullptr;
}

QVariantList Backend::bookmarks() const {
    QVariantList list;
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return list;
    for (const Entry::Bookmark &mark : m_library.first().bookmarks)
        list << QVariantMap{{"section", mark.section}, {"position", mark.position},
                            {"text", mark.text},       {"chapter", chapterLabelFor(mark.section)}};
    return list;
}

bool Backend::rightToLeft() const {
    if (!m_book)
        return false;
    if (!m_library.isEmpty() && m_library.first().path == m_book->path && m_library.first().rightToLeft >= 0)
        return m_library.first().rightToLeft == 1;
    return m_book->rightToLeft;
}

void Backend::setTrimMargins(bool trim) {
    if (trim == m_trimMargins)
        return;
    m_trimMargins = trim;
    emit settingsChanged();
    scheduleSave();
}

QRectF Backend::contentBox(int page) {
    if (!m_book || !m_book->paged())
        return QRectF(0, 0, 1, 1);
    // Drawn small, once a page, as it is first shown.
    if (!m_contentBoxes.contains(page))
        m_contentBoxes.insert(page, m_book->contentBox(page));
    return m_contentBoxes.value(page);
}

QVariantList Backend::panelsOf(int page) {
    QVariantList list;
    if (!m_book || !m_book->paged())
        return list;
    if (m_panels.contains(page)) {
        for (const QRectF &r : m_panels.value(page))
            list << QVariantMap{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}};
        return list;
    }
    if (m_panelsAsked.contains(page))
        return list;
    m_panelsAsked.insert(page);
    auto *watcher = new QFutureWatcher<QList<QRectF>>(this);
    connect(watcher, &QFutureWatcher<QList<QRectF>>::finished, this, [this, watcher, page, book = m_book] {
        watcher->deleteLater();
        if (book != m_book)
            return;
        m_panels.insert(page, watcher->result());
        ++m_panelsFound;
        emit panelsChanged();
    });
    watcher->setFuture(QtConcurrent::run([book = m_book, page, rtl = rightToLeft()] { return book->panels(page, rtl); }));
    return list;
}

void Backend::setRightToLeft(bool rightToLeft) {
    Entry *entry = openEntry();
    if (!entry || rightToLeft == this->rightToLeft())
        return;
    entry->rightToLeft = rightToLeft ? 1 : 0;
    // Panels are read in the other order now.
    m_panels.clear();
    m_panelsAsked.clear();
    ++m_panelsFound;
    emit panelsChanged();
    emit bookChanged();
    scheduleSave();
}

QVariantList Backend::highlights() const {
    QVariantList list;
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return list;
    for (const Highlight &h : m_library.first().highlights)
        list << QVariantMap{{"section", h.section}, {"start", h.start},     {"end", h.end},
                            {"text", h.text},       {"note", h.note},       {"color", h.color},
                            {"chapter", chapterLabelFor(h.section)},
                            {"x", h.area.x()},      {"y", h.area.y()},      {"width", h.area.width()},
                            {"height", h.area.height()}};
    return list;
}

int Backend::addHighlight(int start, int end, const QString &text, const QString &color) {
    Entry *entry = openEntry();
    if (!entry || end <= start || !highlightColors().contains(color))
        return -1;
    auto &marks = entry->highlights;
    // What it overlaps gives way, and passes on its note.
    QStringList notes;
    marks.removeIf([&](const Highlight &h) {
        const bool overlaps = h.section == m_section && h.start < end && start < h.end;
        if (overlaps && !h.note.trimmed().isEmpty())
            notes << h.note.trimmed();
        return overlaps;
    });
    marks << Highlight{m_section, start, end, text.trimmed(), notes.join("\n\n"), color, {}};
    std::sort(marks.begin(), marks.end(), [](const Highlight &a, const Highlight &b) {
        return std::tie(a.section, a.start) < std::tie(b.section, b.start);
    });
    emit highlightsChanged();
    scheduleSave();
    return highlightAt(start);
}

int Backend::addPageHighlight(int page, double x, double y, double width, double height, const QString &color) {
    Entry *entry = openEntry();
    const QRectF area(x, y, width, height);
    if (!entry || !m_book || !m_book->paged() || area.isEmpty() || !highlightColors().contains(color))
        return -1;
    auto &marks = entry->highlights;
    // What it overlaps on the page gives way, and passes on its note.
    QStringList notes;
    marks.removeIf([&](const Highlight &h) {
        const bool overlaps = h.section == page && h.area.intersects(area);
        if (overlaps && !h.note.trimmed().isEmpty())
            notes << h.note.trimmed();
        return overlaps;
    });
    marks << Highlight{page, 0, 0, m_book->pageText(page, area).simplified(), notes.join("\n\n"), color, area};
    std::sort(marks.begin(), marks.end(), [](const Highlight &a, const Highlight &b) {
        return std::make_tuple(a.section, a.start, a.area.top()) < std::make_tuple(b.section, b.start, b.area.top());
    });
    emit highlightsChanged();
    scheduleSave();
    return pageHighlightAt(page, area.center().x(), area.center().y());
}

int Backend::pageHighlightAt(int page, double x, double y) const {
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return -1;
    const auto &marks = m_library.first().highlights;
    for (int i = 0; i < marks.size(); ++i)
        if (marks[i].section == page && marks[i].area.contains(QPointF(x, y)))
            return i;
    return -1;
}

QString Backend::pageText(int page, double x, double y, double width, double height) const {
    return m_book ? m_book->pageText(page, QRectF(x, y, width, height)).simplified() : QString();
}

int Backend::highlightAt(int position) const {
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return -1;
    const auto &marks = m_library.first().highlights;
    for (int i = 0; i < marks.size(); ++i)
        if (marks[i].section == m_section && marks[i].area.isEmpty() && marks[i].start <= position && position < marks[i].end)
            return i;
    return -1;
}

void Backend::setHighlightColor(int index, const QString &color) {
    Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->highlights.size() || !highlightColors().contains(color))
        return;
    entry->highlights[index].color = color;
    emit highlightsChanged();
    scheduleSave();
}

void Backend::setHighlightNote(int index, const QString &note) {
    Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->highlights.size())
        return;
    entry->highlights[index].note = note.trimmed();
    emit highlightsChanged();
    scheduleSave();
}

void Backend::removeHighlight(int index) {
    Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->highlights.size())
        return;
    entry->highlights.removeAt(index);
    emit highlightsChanged();
    scheduleSave();
}

void Backend::goToHighlight(int index) {
    const Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->highlights.size())
        return;
    const Highlight mark = entry->highlights[index];
    markPlace();
    goTo(mark.section, 0);
    setTarget(mark.start);
}

void Backend::paintHighlights(QQuickTextDocument *document, const QVariantMap &colors) const {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text || !m_book)
        return;
    // Painted with a mark of its own, so the old paint can be taken off
    // without touching the book's own formatting.
    constexpr int painted = QTextFormat::UserProperty + 10;
    // What is to be painted, and whether this document already shows it: the
    // paint changes the text, which the window answers by painting again.
    QString wanted;
    if (!m_library.isEmpty() && m_library.first().path == m_book->path)
        for (const Highlight &h : m_library.first().highlights)
            if (h.section == m_section)
                wanted += QString("%1-%2-%3;").arg(h.start).arg(h.end).arg(colors.value(h.color).value<QColor>().name());
    if (m_painting || (text->property("omareadPaint").toString() == wanted
                       && text->property("omareadRevision").toInt() == text->revision()))
        return;
    // The window hears of the change while it's being made, and asks again.
    m_painting = true;
    QTextCursor cursor(text);
    cursor.beginEditBlock();
    QList<std::pair<int, int>> old;
    for (QTextBlock block = text->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().boolProperty(painted))
                old << std::pair{it.fragment().position(), it.fragment().position() + it.fragment().length()};
    for (const auto &[from, to] : old) {
        QTextCursor fragment(text);
        fragment.setPosition(from);
        fragment.setPosition(to, QTextCursor::KeepAnchor);
        QTextCharFormat format = fragment.charFormat();
        format.clearBackground();
        format.clearProperty(painted);
        fragment.setCharFormat(format);
    }
    if (!m_library.isEmpty() && m_library.first().path == m_book->path) {
        const int length = text->characterCount() - 1;
        for (const Highlight &h : m_library.first().highlights) {
            if (h.section != m_section || h.start >= length)
                continue;
            QTextCursor range(text);
            range.setPosition(std::clamp(h.start, 0, length));
            range.setPosition(std::clamp(h.end, 0, length), QTextCursor::KeepAnchor);
            QTextCharFormat format;
            format.setBackground(colors.value(h.color).value<QColor>());
            format.setProperty(painted, true);
            range.mergeCharFormat(format);
        }
    }
    cursor.endEditBlock();
    text->setProperty("omareadPaint", wanted);
    text->setProperty("omareadRevision", text->revision());
    m_painting = false;
}

void Backend::copyNotes() {
    const Entry *entry = openEntry();
    if (!entry || !m_book)
        return;
    copyText(m_book->notesMarkdown(entry->highlights));
}

bool Backend::canLookUp() const {
    return !QStandardPaths::findExecutable("sdcv").isEmpty() || !QStandardPaths::findExecutable("dict").isEmpty();
}

void Backend::lookUp(const QString &asked) {
    const QString word = asked.simplified().left(80);
    QString program = QStandardPaths::findExecutable("sdcv");
    QStringList arguments{"--non-interactive", "--utf8-output", "--utf8-input", word};
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable("dict");
        arguments = {word};
    }
    if (word.isEmpty() || program.isEmpty()) {
        emit lookedUp(word, {});
        return;
    }
    auto *process = new QProcess(this);
    // A dictionary server that doesn't answer isn't waited on for long.
    QTimer::singleShot(5000, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this, [this, process, word](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (status != QProcess::NormalExit || code != 0 || text.startsWith("Nothing similar")
            || text.startsWith("No definitions"))
            text.clear();
        emit lookedUp(word, text.left(4000));
    });
    process->start(program, arguments);
}

QString Backend::stateFile() {
    return defaultStateDirectory() + "/library.json";
}

bool Backend::toggleBookmark(double from, double to, const QString &text) {
    Entry *entry = openEntry();
    if (!entry)
        return false;
    auto &marks = entry->bookmarks;
    const auto here = [&](const Entry::Bookmark &mark) {
        return mark.section == m_section && mark.position >= from - 0.0001 && mark.position < to;
    };
    const bool marked = std::any_of(marks.begin(), marks.end(), here);
    if (marked) {
        marks.removeIf(here);
    } else {
        marks << Entry::Bookmark{m_section, std::clamp(from, 0.0, 1.0), text.simplified().left(200)};
        std::sort(marks.begin(), marks.end(), [](const Entry::Bookmark &a, const Entry::Bookmark &b) {
            return std::tie(a.section, a.position) < std::tie(b.section, b.position);
        });
    }
    emit bookmarksChanged();
    scheduleSave();
    return !marked;
}

void Backend::goToBookmark(int index) {
    const Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->bookmarks.size())
        return;
    const Entry::Bookmark mark = entry->bookmarks[index];
    markPlace();
    goTo(mark.section, mark.position);
}

void Backend::removeBookmark(int index) {
    Entry *entry = openEntry();
    if (!entry || index < 0 || index >= entry->bookmarks.size())
        return;
    entry->bookmarks.removeAt(index);
    emit bookmarksChanged();
    scheduleSave();
}

void Backend::markPlace() {
    if (!m_book)
        return;
    // Twice in the same place, as a second jump from where the first landed
    // before reading on, is one place to go back to.
    if (!m_back.isEmpty() && m_back.last().section == m_section && qAbs(m_back.last().position - m_position) < 0.001)
        return;
    m_back << Place{m_section, m_position};
    while (m_back.size() > 50)
        m_back.removeFirst();
    m_forward.clear();
    emit historyChanged();
}

bool Backend::goBack() {
    if (!m_book || m_back.isEmpty())
        return false;
    m_forward << Place{m_section, m_position};
    const Place place = m_back.takeLast();
    goTo(place.section, place.position);
    emit historyChanged();
    return true;
}

bool Backend::goForward() {
    if (!m_book || m_forward.isEmpty())
        return false;
    m_back << Place{m_section, m_position};
    const Place place = m_forward.takeLast();
    goTo(place.section, place.position);
    emit historyChanged();
    return true;
}

bool Backend::followLink(const QString &link) {
    if (!m_book)
        return false;
    QString fragment;
    const int section = m_book->sectionForLink(link, &fragment);
    if (section < 0)
        return false;
    markPlace();
    goTo(section, 0);
    setTarget(m_book->fragmentOffset(section, fragment));
    return true;
}

void Backend::remember() {
    if (!m_book || m_library.isEmpty() || m_library.first().path != m_book->path)
        return;
    m_library.first().section = m_section;
    m_library.first().position = m_position;
    m_library.first().progress = progress();
    // The end of the book's text: it's read.
    if (!m_library.first().finished && m_library.first().progress >= 0.995) {
        m_library.first().finished = true;
        m_library.first().finishedAt = QDateTime::currentMSecsSinceEpoch();
        emit statsChanged();
        emit bookFinished();
        emit libraryChanged();
    }
    scheduleSave();
}

void Backend::find(const QString &words) {
    const QString query = words.simplified();
    if (!m_book || query.isEmpty()) {
        clearSearch();
        return;
    }
    // Back from a search returns to where it began.
    markPlace();
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

void Backend::chooseHit(int index) {
    if (index >= 0 && index < m_hits.size())
        showHit(index);
}

QVariantList Backend::hitList() const {
    QVariantList list;
    for (int i = 0; i < std::min<int>(int(m_hits.size()), listedHits); ++i) {
        const SearchHit &hit = m_hits[i];
        list << QVariantMap{{"section", hit.section},     {"chapter", chapterLabelFor(hit.section)},
                            {"context", hit.context},     {"at", hit.contextAt},
                            {"length", int(m_query.size())}};
    }
    return list;
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

QVariantList Backend::linksIn(QQuickTextDocument *document) const {
    QVariantList links;
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text)
        return links;
    QString href;
    int start = -1, end = -1;
    auto close = [&] {
        if (start >= 0)
            links << QVariantMap{{"start", start}, {"end", end}, {"href", href}};
        start = -1;
    };
    for (QTextBlock block = text->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            const QString target = format.isAnchor() ? format.anchorHref() : QString();
            // One link may run over several fragments, as italics inside it.
            if (!target.isEmpty() && target == href && start >= 0 && fragment.position() == end) {
                end = fragment.position() + fragment.length();
                continue;
            }
            close();
            if (!target.isEmpty()) {
                href = target;
                start = fragment.position();
                end = start + fragment.length();
            } else {
                href.clear();
            }
        }
    }
    close();
    return links;
}

void Backend::loadImages(QQuickTextDocument *document) {
    QTextDocument *text = document ? document->textDocument() : nullptr;
    if (!text || !m_book || m_book->kind != Book::Kind::Epub)
        return;
    static const QString scheme = "image://omaread/";
    bool added = false;
    for (QTextBlock block = text->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextCharFormat format = it.fragment().charFormat();
            if (!format.isImageFormat())
                continue;
            const QString name = format.toImageFormat().name();
            if (!name.startsWith(scheme) || text->resource(QTextDocument::ImageResource, QUrl(name)).isValid())
                continue;
            const QImage image = PageProvider::image(*m_book, name.mid(scheme.size()), {});
            if (image.isNull())
                continue;
            text->addResource(QTextDocument::ImageResource, QUrl(name), image);
            added = true;
        }
    }
    if (added)
        text->markContentsDirty(0, text->characterCount());
}

void Backend::typeset(QQuickTextDocument *document, double lineHeight, double paragraphGap, bool justify) {
    if (document)
        Layout::typeset(document->textDocument(), lineHeight, paragraphGap, justify,
                        m_book && m_book->kind == Book::Kind::Markdown);
}

void Backend::setPageLayout(int width, int height, const QString &family, int pixelSize, double lineHeight,
                            bool justify) {
    const Layout::Page page{width, height, family, pixelSize, lineHeight, justify};
    if (page == m_page)
        return;
    m_page = page;
    // Waits for a resize or a run of key presses to settle before laying out
    // the whole book again.
    m_pageTimer.start();
}

void Backend::countPages() {
    if (m_pageCancel)
        *m_pageCancel = true;
    const int generation = ++m_pageGeneration;
    // Another book: its pages are counted from nothing. The same book laid
    // out anew keeps showing its last count until the new one is in, rather
    // than flashing "Counting pages…" at each resize.
    const bool sameBook = m_book && m_pagesPath == m_book->path;
    if (!sameBook && !m_pages.isEmpty()) {
        m_pages.clear();
        m_pagesBefore.clear();
    }
    m_pagesPath = m_book ? m_book->path : QString();
    m_countingPages = m_book && !m_book->paged() && m_page.width > 0 && m_page.height > 0;
    emit pagesChanged();
    if (!m_countingPages)
        return;
    m_pageCancel = std::make_shared<std::atomic_bool>(false);
    auto *watcher = new QFutureWatcher<QList<Layout::Pages>>(this);
    connect(watcher, &QFutureWatcher<QList<Layout::Pages>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_pageGeneration)
            return;
        m_pages = watcher->result();
        m_countingPages = false;
        m_pagesBefore.clear();
        int before = 0;
        for (const auto &section : std::as_const(m_pages)) {
            m_pagesBefore << before;
            before += int(section.tops.size());
        }
        emit pagesChanged();
    });
    watcher->setFuture(QtConcurrent::run([book = m_book, page = m_page, cancel = m_pageCancel] {
        return Layout::paginate(*book, page, cancel.get());
    }));
}

int Backend::pageCount() const {
    int count = 0;
    for (const auto &section : m_pages)
        count += int(section.tops.size());
    return count;
}

QVariantMap Backend::pagePreview(int page) const {
    if (!m_book)
        return {};
    int section = page - 1, index = 0;
    double position = 0;
    const bool counted = !m_book->paged() && pageCount() > 0;
    if (counted) {
        page = std::clamp(page, 1, pageCount());
        section = int(std::upper_bound(m_pagesBefore.begin(), m_pagesBefore.end(), page - 1) - m_pagesBefore.begin()) - 1;
        index = page - 1 - m_pagesBefore.value(section);
        const Layout::Pages &pages = m_pages[section];
        const double scroll = pages.height - m_page.height;
        position = scroll > 0 ? std::clamp(pages.tops.value(index) / scroll, 0.0, 1.0) : 0;
    }
    section = std::clamp(section, 0, m_book->sectionCount() - 1);
    QString text;
    if (!m_book->paged()) {
        const QString all = m_book->plainText(section);
        const int from = counted ? m_pages[section].characters.value(index) : 0;
        text = all.mid(from, 360);
        for (QChar &c : text)
            if (c == QChar::ParagraphSeparator || c == QChar::LineSeparator || c == '\n')
                c = ' ';
        text = text.simplified();
    }
    return {{"section", section}, {"position", position}, {"chapter", chapterLabelFor(section)}, {"text", text}};
}

void Backend::goToPage(int page) {
    const QVariantMap preview = pagePreview(page);
    if (preview.isEmpty())
        return;
    markPlace();
    goTo(preview.value("section").toInt(), preview.value("position").toDouble());
}

int Backend::pageAt(int section, double contentY) const {
    if (section < 0 || section >= m_pages.size() || m_pages[section].tops.isEmpty())
        return 0;
    const QList<double> &starts = m_pages[section].tops;
    // The last page that starts at or above the top of the view.
    const auto it = std::upper_bound(starts.begin(), starts.end(), contentY + 1);
    return m_pagesBefore.value(section) + std::max(1, int(it - starts.begin()));
}
