#pragma once

#include <QMutex>
#include <QQuickImageProvider>

#include <memory>

#include "book.h"

// The book the window shows, shared by the backend, which replaces it, and the
// image provider, which draws from it on Qt Quick's loader threads.
struct OpenBook {
    QMutex mutex;
    std::shared_ptr<const Book> book;

    std::shared_ptr<const Book> get() {
        QMutexLocker lock(&mutex);
        return book;
    }
    void set(std::shared_ptr<const Book> next) {
        QMutexLocker lock(&mutex);
        book = std::move(next);
    }
};

// Serves image://omaread/page/<book>/<page>[/<background>/<foreground>] for a
// paged book's pages, tinted when the colors are given, and
// image://omaread/res/<book>/<hex path> for an EPUB's images. A request for a
// book that is no longer open gets an empty image.
class PageProvider : public QQuickImageProvider {
public:
    explicit PageProvider(std::shared_ptr<OpenBook> open)
        : QQuickImageProvider(QQuickImageProvider::Image, QQuickImageProvider::ForceAsynchronousImageLoading),
          m_open(std::move(open)) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    // The image for an id, as requestImage draws it; tests call it directly.
    static QImage image(const Book &book, const QString &id, const QSize &requestedSize);

private:
    std::shared_ptr<OpenBook> m_open;
};
