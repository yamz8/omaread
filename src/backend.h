#pragma once

#include <QColor>
#include <QFutureWatcher>
#include <QObject>
#include <QRectF>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <memory>

#include "book.h"
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
    // The matches on the pages shown, each {page, x, y, width, height, current}.
    Q_PROPERTY(QVariantList pageHits READ pageHits NOTIFY hitChanged)

    // The width the text is laid out in, so an EPUB's images fit inside it.
    Q_PROPERTY(int readingWidth READ readingWidth WRITE setReadingWidth NOTIFY readingWidthChanged)
    // The reading text's size as a multiple of the desktop's text size.
    Q_PROPERTY(double textScale READ textScale WRITE setTextScale NOTIFY settingsChanged)
    Q_PROPERTY(bool tintPages READ tintPages WRITE setTintPages NOTIFY settingsChanged)
    // Two pages side by side, as a printed book or comic opens.
    Q_PROPERTY(bool spread READ spread WRITE setSpread NOTIFY settingsChanged)
    // How a page turns: one of transitions, "slide" by default.
    Q_PROPERTY(QString transition READ transition WRITE setTransition NOTIFY settingsChanged)
    Q_PROPERTY(QStringList transitions READ transitions CONSTANT)

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
    QVariantList pageHits() const;

    int readingWidth() const { return m_readingWidth; }
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

    Q_INVOKABLE void openUrl(const QUrl &url);
    Q_INVOKABLE void openFile(const QString &path, const QString &password = {});
    Q_INVOKABLE void openRecent(int index);
    Q_INVOKABLE void forget(int index);
    Q_INVOKABLE void pickFile();
    Q_INVOKABLE void close();
    Q_INVOKABLE void goTo(int section, double position = 0);
    Q_INVOKABLE void goToEntry(int tocIndex);
    // Follows a link inside the book, and says whether it was one.
    Q_INVOKABLE bool followLink(const QString &link);
    Q_INVOKABLE void dismissError();
    Q_INVOKABLE void unlock(const QString &password);
    Q_INVOKABLE void cancelPassword();

    // Searches the whole book, and goes to the first match from here on.
    Q_INVOKABLE void find(const QString &words);
    Q_INVOKABLE void nextHit();
    Q_INVOKABLE void previousHit();
    Q_INVOKABLE void clearSearch();
    // Copies the words inside an area of a page, given as fractions of it, and
    // says whether there were any.
    Q_INVOKABLE bool copyPageText(int page, double x, double y, double width, double height);
    Q_INVOKABLE void copyText(const QString &text);
    // Opens up a chapter's lines for reading, as a factor of their height.
    // TextEdit has no line height of its own, so it is set on every block.
    Q_INVOKABLE void spaceLines(QQuickTextDocument *document, double factor, double paragraphGap = 0);

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
    QVariantList m_links;

    QString m_query;
    bool m_searching = false;
    QList<SearchHit> m_hits;
    int m_hitIndex = -1;
    int m_searchGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_searchCancel;

    int m_readingWidth = 0;
    double m_textScale = defaultTextScale;
    bool m_tintPages = false;
    bool m_spread = false;
    QString m_transition = "slide";
    QTimer m_saveTimer;
};
