#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <memory>

#include "book.h"
#include "pageprovider.h"

class FilePicker;

// The reader's state: the library of books read before and where each was
// left, the open book, and the place in it. The window only lays it out.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

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
    // How far down the section the reader is, from 0 to 1.
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)

    // The width the text is laid out in, so an EPUB's images fit inside it.
    Q_PROPERTY(int readingWidth READ readingWidth WRITE setReadingWidth NOTIFY readingWidthChanged)
    Q_PROPERTY(int textSize READ textSize WRITE setTextSize NOTIFY settingsChanged)
    Q_PROPERTY(bool tintPages READ tintPages WRITE setTintPages NOTIFY settingsChanged)

public:
    // Tests pass a temp state directory and a fake picker; by default the
    // library lives in ~/.local/state/omaread and files come from the portal.
    explicit Backend(const QString &stateDirectory = {}, FilePicker *picker = nullptr, QObject *parent = nullptr);
    ~Backend() override;

    // For the engine, which takes ownership: draws the open book's pages.
    PageProvider *createPageProvider() const { return new PageProvider(m_open); }

    QVariantList library() const;
    bool loading() const { return m_loading; }
    QString error() const;

    bool isOpen() const { return bool(m_book); }
    int bookId() const { return m_book ? m_book->id : 0; }
    QString title() const { return m_book ? m_book->title : QString(); }
    QString author() const { return m_book ? m_book->author : QString(); }
    QString kind() const;
    bool paged() const { return m_book && m_book->paged(); }
    int sectionCount() const { return m_book ? m_book->sectionCount() : 0; }
    QVariantList toc() const;
    QVariantList aspects() const;
    int textFormat() const;
    QUrl baseUrl() const;

    int section() const { return m_section; }
    int tocIndex() const;
    QString text() const { return m_text; }
    double position() const { return m_position; }
    void setPosition(double position);
    double progress() const;

    int readingWidth() const { return m_readingWidth; }
    void setReadingWidth(int width);
    int textSize() const { return m_textSize; }
    void setTextSize(int size);
    bool tintPages() const { return m_tintPages; }
    void setTintPages(bool tint);

    Q_INVOKABLE void openUrl(const QUrl &url);
    Q_INVOKABLE void openFile(const QString &path);
    Q_INVOKABLE void openRecent(int index);
    Q_INVOKABLE void forget(int index);
    Q_INVOKABLE void pickFile();
    Q_INVOKABLE void close();
    Q_INVOKABLE void goTo(int section, double position = 0);
    // Follows a link inside the book, and says whether it was one.
    Q_INVOKABLE bool followLink(const QString &link);
    Q_INVOKABLE void dismissError();

    // Writes the library now rather than after the pause that gathers changes.
    void save();

public slots:
    // The error is translated when it is read, so a language switch shows it anew.
    void retranslate() { emit errorChanged(); }

signals:
    void libraryChanged();
    void loadingChanged();
    void errorChanged();
    void bookChanged();
    void sectionChanged();
    void textChanged();
    void positionChanged();
    void readingWidthChanged();
    void settingsChanged();

private:
    enum class Problem { None, Missing, Unreadable, Unsupported, Locked, Empty, Picker };
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

    void loaded(int generation, const Loaded &result);
    void setProblem(Problem problem, const QString &detail = {});
    void updateText();
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
    QString m_pendingPath;
    int m_section = 0;
    double m_position = 0;
    QString m_text;
    int m_readingWidth = 0;
    int m_textSize = 20;
    bool m_tintPages = false;
    QTimer m_saveTimer;
};
