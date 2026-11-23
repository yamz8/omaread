#include "pageprovider.h"

QImage PageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    QImage result;
    if (const auto book = m_open->get())
        result = image(*book, id, requestedSize);
    // A request for a book that has since been closed or replaced is
    // answered, blank, rather than failed: its image is about to be replaced.
    if (result.isNull()) {
        result = QImage(1, 1, QImage::Format_ARGB32);
        result.fill(Qt::transparent);
    }
    if (size)
        *size = result.size();
    return result;
}

QImage PageProvider::image(const Book &book, const QString &id, const QSize &requestedSize) {
    const QStringList parts = id.split('/');
    if (parts.size() < 3 || parts[1].toInt() != book.id)
        return {};
    if (parts[0] == "page") {
        const QColor background = parts.size() >= 5 ? QColor("#" + parts[3]) : QColor();
        const QColor foreground = parts.size() >= 5 ? QColor("#" + parts[4]) : QColor();
        return book.renderPage(parts[2].toInt(), requestedSize, background, foreground);
    }
    if (parts[0] == "res" || parts[0] == "ink") {
        QImage image = book.resource(QString::fromUtf8(QByteArray::fromHex(parts[2].toLatin1())));
        if (!image.isNull() && requestedSize.isValid() && !requestedSize.isEmpty()
            && (image.width() > requestedSize.width() || image.height() > requestedSize.height()))
            image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (parts[0] == "ink" && parts.size() >= 4 && !image.isNull())
            image = Book::inked(image, QColor("#" + parts[3]));
        return image;
    }
    return {};
}
