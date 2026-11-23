#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <memory>

#include "book.h"
#include "layout.h"
#include "pageprovider.h"

class FilePicker;
class QQuickTextDocument;

// The reader's state: the library of books read before and where each was
// left, the open book, the place in it, and a search through it. The window
// only lays it out.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    // The file name of a PDF that needs a password, while the window asks for one.
    Q_PROPERTY(QString passwordFor READ passwordFor NOTIFY passwordChanged)
    Q_PROPERTY(bool passwordWrong READ passwordWrong NOTIFY passwordChanged)

    Q_PROPERTY(bool open READ isOpen NOTIFY bookChanged)
    Q_PROPERTY(int bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString author READ author NOTIFY bookChanged)
    Q_PROPERTY(QString kind READ kind NOTIFY bookChanged)
    Q_PROPERTY(bool paged READ paged NOTIFY bookChanged)
    Q_PROPERTY(int sectionCount READ sectionCount NOTIFY bookChanged)
    Q_PROPERTY(QVariantList toc READ toc NOTIFY bookChanged)
    Q_PROPERTY(QVariantList aspects READ aspects NOTIFY bookChanged)
    Q_PROPERTY(int textFormat READ textFormat NOTIFY bookChanged)
    Q_PROPERTY(QUrl baseUrl READ baseUrl NOTIFY bookChanged)

    Q_PROPERTY(int section READ section NOTIFY sectionChanged)
    Q_PROPERTY(int tocIndex READ tocIndex NOTIFY sectionChanged)
    Q_PROPERTY(QString text READ text NOTIFY textChanged)
    // The links on the pages shown, each {page, x, y, width, height, target, url}
    // with the area as fractions of its page.
    Q_PROPERTY(QVariantList links READ links NOTIFY sectionChanged)
    // How far down the section the reader is, from 0 to 1.
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)
    // Where in a flowing section's text to bring into view, as a count of
    // characters, after following a link to a place inside it; -1 for none.
    Q_PROPERTY(int target READ target NOTIFY targetChanged)

    Q_PROPERTY(QString query READ query NOTIFY searchChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
    Q_PROPERTY(int hitCount READ hitCount NOTIFY searchChanged)
    Q_PROPERTY(int hitIndex READ hitIndex NOTIFY hitChanged)
    Q_PROPERTY(int hitSection READ hitSection NOTIFY hitChanged)
    Q_PROPERTY(int hitOccurrence READ hitOccurrence NOTIFY hitChanged)
    // Where the current match starts in its section's text, or -1.
    Q_PROPERTY(int hitOffset READ hitOffset NOTIFY hitChanged)
    // The first matches, each {section, chapter, context, at, length}, for a
    // list to choose from.
    Q_PROPERTY(QVariantList hitList READ hitList NOTIFY searchChanged)
    // The matches on the pages shown, each {page, x, y, width, height, current}.
    Q_PROPERTY(QVariantList pageHits READ pageHits NOTIFY hitChanged)

    // The width the text is laid out in, so an EPUB's images fit inside it.
    Q_PROPERTY(int readingWidth READ readingWidth WRITE setReadingWidth NOTIFY readingWidthChanged)
    // The reading text's size in pixels, which a book's own spacing is measured in.
    Q_PROPERTY(int readingSize READ readingSize WRITE setReadingSize NOTIFY readingWidthChanged)
    // The reading text's size as a multiple of the desktop's text size.
    Q_PROPERTY(double textScale READ textScale WRITE setTextScale NOTIFY settingsChanged)
    Q_PROPERTY(bool tintPages READ tintPages WRITE setTintPages NOTIFY settingsChanged)
    // Two pages side by side, as a printed book or comic opens.
    Q_PROPERTY(bool spread READ spread WRITE setSpread NOTIFY settingsChanged)
    // How a page turns: one of transitions, "slide" by default.
    Q_PROPERTY(QString transition READ transition WRITE setTransition NOTIFY settingsChanged)
    Q_PROPERTY(QStringList transitions READ transitions CONSTANT)
    // Where the reader is, as the book names it: the part and the chapter,
    // "Part II · V", leaving out an entry that only repeats the book's title.
    Q_PROPERTY(QString chapterLabel READ chapterLabel NOTIFY sectionChanged)
    // Minutes left at the reader's pace, in this section and in the whole
    // book's text; -1 when the book has no words to count, as a PDF or comic.
    // The book's pages at the window's size and the reading settings, as Space
    // turns them; 0 while they are being counted, and for a PDF or comic.
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged)
    // The open book's bookmarks, in reading order, each {section, position,
    // text, chapter}: the place, the first words there, and its chapter.
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    // The open book's highlights, in reading order, each {section, start, end,
    // text, note, color, chapter}.
    Q_PROPERTY(QVariantList highlights READ highlights NOTIFY highlightsChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(int minutesLeftInChapter READ minutesLeftInChapter NOTIFY positionChanged)
    Q_PROPERTY(int minutesLeftInBook READ minutesLeftInBook NOTIFY positionChanged)
    // The reader's pace, learned from how long a screen takes to read.
    Q_PROPERTY(double wordsPerMinute READ wordsPerMinute NOTIFY positionChanged)
    // What the footer's right side shows: "chapter" or "book" time left,
    // "page", "percent", or "none". P steps through them.
    Q_PROPERTY(QString progressShows READ progressShows WRITE setProgressShows NOTIFY settingsChanged)
    // How the library shows its books: "grid" of covers or "list"; in
    // "recent", "title", "author" or "progress" order; and "all", "reading",
    // "unread" or "finished" ones.
    Q_PROPERTY(QString libraryView READ libraryView WRITE setLibraryView NOTIFY settingsChanged)
    Q_PROPERTY(QString librarySort READ librarySort WRITE setLibrarySort NOTIFY settingsChanged)
    Q_PROPERTY(QString libraryFilter READ libraryFilter WRITE setLibraryFilter NOTIFY settingsChanged)
    // How a flowing book's text is set, for every book: "serif" or "sans";
    // "compact", "normal" or "relaxed" lines; a "narrow", "medium" or "wide"
    // column; and ragged or justified lines.
    Q_PROPERTY(QString typeface READ typeface WRITE setTypeface NOTIFY settingsChanged)
    Q_PROPERTY(QString lineSpacing READ lineSpacing WRITE setLineSpacing NOTIFY settingsChanged)
    Q_PROPERTY(QString columnWidth READ columnWidth WRITE setColumnWidth NOTIFY settingsChanged)
    Q_PROPERTY(bool justify READ justify WRITE setJustify NOTIFY settingsChanged)

public:
    static constexpr double defaultTextScale = 1.5;

    // Tests pass a temp state directory and a fake picker; by default the
    // library lives in ~/.local/state/omaread and files come from the portal.
    explicit Backend(const QString &stateDirectory = {}, FilePicker *picker = nullptr, QObject *parent = nullptr);
    ~Backend() override;

    // For the engine, which takes ownership: draws the open book's pages.
    PageProvider *createPageProvider() const { return new PageProvider(m_open); }

    QVariantList library() const;
    bool loading() const { return m_loading; }
    QString error() const;
    QString passwordFor() const;
    bool passwordWrong() const { return m_passwordWrong; }

    bool isOpen() const { return bool(m_book); }
    int bookId() const { return m_book ? m_book->id : 0; }
    QString title() const { return m_book ? m_book->title : QString(); }
    QString author() const { return m_book ? m_book->author : QString(); }
    QString kind() const { return m_book ? m_book->kindName() : QString(); }
    bool paged() const { return m_book && m_book->paged(); }
    int sectionCount() const { return m_book ? m_book->sectionCount() : 0; }
    QVariantList toc() const;
    QVariantList aspects() const;
    int textFormat() const;
    QUrl baseUrl() const;

    int section() const { return m_section; }
    int tocIndex() const;
    QString text() const { return m_text; }
    QVariantList links() const { return m_links; }
    double position() const { return m_position; }
    void setPosition(double position);
    double progress() const;
    int target() const { return m_target; }

    QString query() const { return m_query; }
    bool searching() const { return m_searching; }
    int hitCount() const { return int(m_hits.size()); }
    int hitIndex() const { return m_hitIndex; }
    int hitSection() const { return m_hitIndex >= 0 ? m_hits[m_hitIndex].section : -1; }
    int hitOccurrence() const { return m_hitIndex >= 0 ? m_hits[m_hitIndex].occurrence : -1; }
    int hitOffset() const { return m_hitIndex >= 0 ? m_hits[m_hitIndex].offset : -1; }
    QVariantList hitList() const;
    static constexpr int listedHits = 300;
    // The part and chapter a section is in, as the footer names them.
    QString chapterLabelFor(int section) const;
    QVariantList pageHits() const;

    int readingWidth() const { return m_readingWidth; }
    int readingSize() const { return m_readingSize; }
    void setReadingSize(int size);
    void setReadingWidth(int width);
    double textScale() const { return m_textScale; }
    void setTextScale(double scale);
    bool tintPages() const { return m_tintPages; }
    void setTintPages(bool tint);
    // The color an EPUB's ink images are drawn in: the theme's text color.
    void setInk(const QColor &ink);
    bool spread() const { return m_spread; }
    void setSpread(bool spread);
    QString transition() const { return m_transition; }
    void setTransition(const QString &transition);
    static QStringList transitions() { return {"slide", "flip", "cover", "fade", "none"}; }
    QString chapterLabel() const;
    int pageCount() const;
    QVariantList bookmarks() const;
    QVariantList highlights() const;
    bool canGoBack() const { return !m_back.isEmpty(); }
    bool canGoForward() const { return !m_forward.isEmpty(); }
    int minutesLeftInChapter() const;
    int minutesLeftInBook() const;
    double wordsPerMinute() const { return m_wordsPerMinute; }
    // A screen's worth of reading: how long it took, and how much of the
    // section it held. Pauses and skims are left out.
    Q_INVOKABLE void noteReading(double seconds, double fraction);
    QString progressShows() const { return m_progressShows; }
    void setProgressShows(const QString &shows);
    static QStringList progressModes() { return {"chapter", "book", "page", "percent", "none"}; }
    static constexpr double defaultWordsPerMinute = 250;
    QString libraryView() const { return m_libraryView; }
    void setLibraryView(const QString &view) { setChoice(&m_libraryView, view, {"grid", "list"}); }
    QString librarySort() const { return m_librarySort; }
    void setLibrarySort(const QString &sort) { setChoice(&m_librarySort, sort, {"recent", "title", "author", "progress"}); }
    QString libraryFilter() const { return m_libraryFilter; }
    void setLibraryFilter(const QString &filter) {
        setChoice(&m_libraryFilter, filter, {"all", "reading", "unread", "finished"});
    }
    QString typeface() const { return m_typeface; }
    void setTypeface(const QString &typeface);
    QString lineSpacing() const { return m_lineSpacing; }
    void setLineSpacing(const QString &spacing);
    QString columnWidth() const { return m_columnWidth; }
    void setColumnWidth(const QString &width);
    bool justify() const { return m_justify; }
    void setJustify(bool justify);
    static QStringList typefaces() { return {"serif", "sans"}; }
    static QStringList lineSpacings() { return {"compact", "normal", "relaxed"}; }
    static QStringList columnWidths() { return {"narrow", "medium", "wide"}; }

    Q_INVOKABLE void openUrl(const QUrl &url);
    Q_INVOKABLE void openFile(const QString &path, const QString &password = {});
    Q_INVOKABLE void openRecent(int index);
    // Forgets a book by its path, as the library lists it.
    Q_INVOKABLE void forgetPath(const QString &path);
    // Marks a book read to the end, or not.
    Q_INVOKABLE void setFinished(const QString &path, bool finished);
    // The folders whose books join the library by themselves: ~/Books.
    QStringList folders() const { return m_folders; }
    void setFolders(const QStringList &folders);
    Q_INVOKABLE void forget(int index);
    Q_INVOKABLE void pickFile();
    Q_INVOKABLE void close();
    Q_INVOKABLE void goTo(int section, double position = 0);
    Q_INVOKABLE void goToEntry(int tocIndex);
    // Follows a link inside the book, and says whether it was one.
    Q_INVOKABLE bool followLink(const QString &link);
    // The places left by jumps (the contents, a link, Go to, a search), to
    // go back to with Backspace and on to again, as a browser does.
    Q_INVOKABLE void markPlace();
    // Bookmarks the page on view, from position from to position to of the
    // section, or removes the ones on it. Returns whether it is now marked.
    Q_INVOKABLE bool toggleBookmark(double from, double to, const QString &text);
    Q_INVOKABLE void goToBookmark(int index);
    // Highlights in the section on view, by character positions in its text.
    // A new one takes the place of any it overlaps, keeping their notes.
    Q_INVOKABLE int addHighlight(int start, int end, const QString &text, const QString &color);
    // The highlight in this section at a character position, or -1.
    Q_INVOKABLE int highlightAt(int position) const;
    Q_INVOKABLE void setHighlightColor(int index, const QString &color);
    Q_INVOKABLE void setHighlightNote(int index, const QString &note);
    Q_INVOKABLE void removeHighlight(int index);
    Q_INVOKABLE void goToHighlight(int index);
    // Draws this section's highlights in a chapter's text, colors by name.
    Q_INVOKABLE void paintHighlights(QQuickTextDocument *document, const QVariantMap &colors) const;
    // The open book's highlights and notes as Markdown, copied to the clipboard.
    Q_INVOKABLE void copyNotes();
    // Looks a word up in a dictionary installed here, sdcv's or dictd's; the
    // answer comes in lookedUp, with an empty definition when there was none.
    Q_INVOKABLE void lookUp(const QString &word);
    // Whether a dictionary is installed at all.
    Q_INVOKABLE bool canLookUp() const;
    static QStringList highlightColors() { return {"yellow", "green", "blue", "pink"}; }
    // Where the library is kept, for the commands, which read it as it is.
    static QString stateFile();
    Q_INVOKABLE void removeBookmark(int index);
    Q_INVOKABLE bool goBack();
    Q_INVOKABLE bool goForward();
    // The footnote or endnote a link inside the book points at, as HTML to
    // show in place, or an empty string when it points at something else.
    Q_INVOKABLE QString note(const QString &link) const;
    Q_INVOKABLE void dismissError();
    Q_INVOKABLE void unlock(const QString &password);
    Q_INVOKABLE void cancelPassword();

    // Searches the whole book, and goes to the first match from here on.
    Q_INVOKABLE void find(const QString &words);
    Q_INVOKABLE void nextHit();
    Q_INVOKABLE void chooseHit(int index);
    Q_INVOKABLE void previousHit();
    Q_INVOKABLE void clearSearch();
    // Copies the words inside an area of a page, given as fractions of it, and
    // says whether there were any.
    Q_INVOKABLE bool copyPageText(int page, double x, double y, double width, double height);
    Q_INVOKABLE void copyText(const QString &text);
    // Opens up a chapter's lines for reading, as a factor of their height.
    // TextEdit has no line height of its own, so it is set on every block.
    // Sets a flowing chapter's lines: their height as a multiple of the text
    // size, the gap after a Markdown paragraph, and justified or ragged.
    Q_INVOKABLE void typeset(QQuickTextDocument *document, double lineHeight, double paragraphGap = 0,
                             bool justify = false);
    // What the window shows a flowing book's text in. The book's pages are
    // counted for it in the background, again whenever it changes.
    Q_INVOKABLE void setPageLayout(int width, int height, const QString &family, int pixelSize, double lineHeight,
                                   bool justify);
    // The page at a scroll offset in a section, counting from the book's
    // first, or 0 while the pages are still being counted.
    Q_INVOKABLE int pageAt(int section, double contentY) const;
    // What a page holds, for skimming: {section, position, chapter, text},
    // its first words. Pages count from 1; a book whose pages are still being
    // counted, and a PDF or comic, number its sections instead.
    Q_INVOKABLE QVariantMap pagePreview(int page) const;
    // Goes to a page, leaving a place to come back to.
    Q_INVOKABLE void goToPage(int page);
    // Puts a chapter's images into its document as it arrives. Left to the
    // TextEdit, an image it hasn't drawn before loads late and is never shown.
    Q_INVOKABLE void loadImages(QQuickTextDocument *document);
    // The links in a chapter's text, in reading order, each {start, end, href}
    // by character position, so the keyboard can reach them.
    Q_INVOKABLE QVariantList linksIn(QQuickTextDocument *document) const;

    // Writes the library now rather than after the pause that gathers changes.
    void save();

public slots:
    // The error is translated when it is read, so a language switch shows it anew.
    void retranslate() { emit errorChanged(); }

signals:
    void libraryChanged();
    void loadingChanged();
    void errorChanged();
    void passwordChanged();
    void bookChanged();
    void sectionChanged();
    void textChanged();
    void positionChanged();
    // Just before new text: the window stops taking its scroll for the
    // reader's place until the new text is laid out and the place restored.
    void textAboutToChange();
    void pagesChanged();
    void historyChanged();
    void bookmarksChanged();
    void highlightsChanged();
    void lookedUp(const QString &word, const QString &definition);
    // The book open was just read to its end.
    void bookFinished();
    void targetChanged();
    void searchChanged();
    void hitChanged();
    void readingWidthChanged();
    void settingsChanged();

private:
    enum class Problem { None, Missing, Unreadable, Unsupported, Empty, Picker };
    struct Entry {
        QString path, title, author, kind;
        int section = 0, sections = 0;
        double position = 0;
        qint64 opened = 0;
        // How much of the book's text has been read, as the open book counts
        // it; -1 for an entry saved before it was kept.
        double progress = -1;
        struct Bookmark {
            int section = 0;
            double position = 0;
            QString text;
        };
        QList<Bookmark> bookmarks;
        QList<Highlight> highlights;
        bool finished = false;
        // When a book found in a watched folder arrived there; 0 for one opened.
        qint64 added = 0;
        // Forgotten, though its file stays in a watched folder.
        bool hidden = false;
    };
    struct Loaded {
        std::shared_ptr<Book> book;
        Book::Error error = Book::Error::None;
    };

    void loaded(int generation, const QString &path, const Loaded &result);
    void setProblem(Problem problem, const QString &detail = {});
    void setTarget(int target);
    void updateText();
    void updateLinks();
    void showHit(int index);
    void remember();
    void scheduleSave();
    void readState();

    QString m_stateFile;
    FilePicker *m_picker = nullptr;
    QList<Entry> m_library;
    std::shared_ptr<OpenBook> m_open = std::make_shared<OpenBook>();
    std::shared_ptr<Book> m_book;
    int m_nextId = 1;
    int m_generation = 0;
    bool m_loading = false;
    Problem m_problem = Problem::None;
    QString m_problemDetail;
    QString m_passwordPath;
    bool m_passwordWrong = false;
    int m_section = 0;
    double m_position = 0;
    int m_target = -1;
    QString m_text;
    QColor m_ink;
    struct Place {
        int section = 0;
        double position = 0;
    };
    QList<Place> m_back, m_forward;
    mutable bool m_painting = false;
    // Covers, as small images in a cache, and the book whose cover is being made.
    QString m_coverDirectory;
    QSet<QString> m_coverTried;
    bool m_coverBusy = false;
    QString coverFile(const QString &path) const;
    void fillCovers();
    void saveCover(const std::shared_ptr<Book> &book);
    // Watched folders.
    QStringList m_folders;
    QFileSystemWatcher m_watcher;
    QTimer m_scanTimer;
    void scanFolders();
    Entry *openEntry();
    Layout::Page m_page;
    QList<Layout::Pages> m_pages;
    QList<int> m_pagesBefore;
    QTimer m_pageTimer;
    int m_pageGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_pageCancel;
    void countPages();
    QVariantList m_links;

    QString m_query;
    bool m_searching = false;
    QList<SearchHit> m_hits;
    int m_hitIndex = -1;
    int m_searchGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_searchCancel;

    int m_readingWidth = 0;
    int m_readingSize = 18;
    double m_textScale = defaultTextScale;
    bool m_tintPages = false;
    // A book reads best in a serif, as printed books are set.
    double m_wordsPerMinute = defaultWordsPerMinute;
    QString m_progressShows = "chapter";
    QString m_libraryView = "grid", m_librarySort = "recent", m_libraryFilter = "all";
    void setChoice(QString *setting, const QString &value, const QStringList &choices);
    QString m_typeface = "serif";
    QString m_lineSpacing = "normal";
    QString m_columnWidth = "medium";
    bool m_justify = false;
    bool m_spread = false;
    QString m_transition = "slide";
    QTimer m_saveTimer;
};
