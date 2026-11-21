#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

// Follows the Omarchy theme live: its whole palette, whether it is light or
// dark, Hyprland's window rounding as radius, and the desktop's text size.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    // The desktop's text size in px, as `omarchy display text size` sets it
    // (12 px by default, the terminals' 9 pt).
    Q_PROPERTY(int textSize READ textSize NOTIFY changed)

public:
    // Tests pass a temp directory, a fake hyprctl, and their own event socket.
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &hyprlandEvents = {});
    ~Theme() override;

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const;
    QColor background() const { return color("background"); }
    QColor foreground() const { return color("foreground"); }
    QVariantMap colors() const;
    bool light() const { return m_light; }
    int radius() const { return m_radius; }
    int textSize() const { return m_textSize; }

    // `base-size` under `[font]` in a shell.toml, or 0 when it isn't set.
    static int readBaseSize(const QString &path);

    struct Palette {
        QHash<QString, QColor> colors;
        bool light = false;
    };
    static Palette readThemeColors(const QString &path);

signals:
    void changed();

private:
    QColor color(const QString &key) const;
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_shellConfig;
    int m_textSize = 12;
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    bool m_roundingAgain = false;
    QProcess m_roundingProcess;
    QTimer m_roundingTimeout;
    QLocalSocket m_events;
    QTimer m_reconnect;
};
