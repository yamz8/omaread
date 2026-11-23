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
#include <QTextList>
#include <QTextDocument>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

#include "layout.h"
#include "portalfilepicker.h"
#include "stylesheet.h"

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
    m_pageTimer.setSingleShot(true);
    m_pageTimer.setInterval(250);
    connect(&m_pageTimer, &QTimer::timeout, this, &Backend::countPages);
    connect(m_picker, &FilePicker::openSelected, this, &Backend::openUrl);
    connect(m_picker, &FilePicker::failed, this, [this](const QString &message) { setProblem(Problem::Picker, message); });
    readState();
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
    m_wordsPerMinute = std::clamp(state.value("wordsPerMinute").toDouble(defaultWordsPerMinute), 80.0, 900.0);
    if (const QString value = state.value("progressShows").toString(); progressModes().contains(value))
        m_progressShows = value;
    for (const QJsonValue &value : state.value("books").toArray()) {
        const QJsonObject book = value.toObject();
        Entry entry{book.value("path").toString(), book.value("title").toString(), book.value("author").toString(),
                    book.value("kind").toString(), book.value("section").toInt(), book.value("sections").toInt(),
                    std::clamp(book.value("position").toDouble(), 0.0, 1.0), qint64(book.value("opened").toDouble()),
                    book.value("progress").toDouble(-1)};
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
                                 {"progress", entry.progress}});
    const QJsonObject state{
        {"textScale", m_textScale}, {"tintPages", m_tintPages}, {"spread", m_spread}, {"transition", m_transition},
        {"typeface", m_typeface},   {"lineSpacing", m_lineSpacing}, {"columnWidth", m_columnWidth},
        {"justify", m_justify},     {"wordsPerMinute", m_wordsPerMinute},
        {"progressShows", m_progressShows}, {"books", books}};
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
            entry.progress >= 0 ? entry.progress
            : entry.sections <= 0 ? 0
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
    if (!m_book)
        return {};
    int index = -1;
    for (int i = 0; i < m_book->toc.size(); ++i)
        if (m_book->toc[i].section <= section)
            index = i;
    if (index < 0)
        return {};
    // The entry and the ones it sits under, nearest first.
    QStringList chain;
    int depth = m_book->toc[index].depth + 1;
    for (int i = index; i >= 0 && depth > 0; --i) {
        const TocEntry &entry = m_book->toc[i];
        if (entry.depth >= depth)
            continue;
        depth = entry.depth;
        if (entry.title.compare(m_book->title, Qt::CaseInsensitive) != 0)
            chain.prepend(entry.title);
    }
    while (chain.size() > 2)
        chain.removeFirst();
    return chain.join(QStringLiteral("  ·  "));
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
    const auto it = std::find_if(m_library.begin(), m_library.end(),
                                 [&](const Entry &entry) { return entry.path == m_book->path; });
    if (it != m_library.end()) {
        m_section = std::clamp(it->section, 0, m_book->sectionCount() - 1);
        m_position = it->position;
        m_library.erase(it);
    }
    m_library.prepend(Entry{m_book->path, m_book->title, m_book->author, m_book->kindName(), m_section,
                            m_book->sectionCount(), m_position, QDateTime::currentMSecsSinceEpoch(), progress()});
    while (m_library.size() > maxLibrary)
        m_library.removeLast();
    scheduleSave();

    setTarget(-1);
    updateLinks();
    m_back.clear();
    m_forward.clear();
    emit historyChanged();
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
    if (!m_pages.isEmpty()) {
        m_pages.clear();
        m_pagesBefore.clear();
        emit pagesChanged();
    }
    if (!m_book || m_book->paged() || m_page.width <= 0 || m_page.height <= 0)
        return;
    m_pageCancel = std::make_shared<std::atomic_bool>(false);
    auto *watcher = new QFutureWatcher<QList<QList<double>>>(this);
    connect(watcher, &QFutureWatcher<QList<QList<double>>>::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_pageGeneration)
            return;
        m_pages = watcher->result();
        m_pagesBefore.clear();
        int before = 0;
        for (const auto &section : std::as_const(m_pages)) {
            m_pagesBefore << before;
            before += int(section.size());
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
        count += int(section.size());
    return count;
}

int Backend::pageAt(int section, double contentY) const {
    if (section < 0 || section >= m_pages.size() || m_pages[section].isEmpty())
        return 0;
    const QList<double> &starts = m_pages[section];
    // The last page that starts at or above the top of the view.
    const auto it = std::upper_bound(starts.begin(), starts.end(), contentY + 1);
    return m_pagesBefore.value(section) + std::max(1, int(it - starts.begin()));
}
