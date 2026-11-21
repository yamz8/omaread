#pragma once

#include <QObject>
#include <QUrl>

// What the backend asks for a file through, so tests can inject a fake in
// place of the portal.
class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openDocument() = 0;

signals:
    void openSelected(const QUrl &url);
    void failed(const QString &message);
};
