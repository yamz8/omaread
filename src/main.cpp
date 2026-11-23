// omaread — reads books and documents: PDF, EPUB, comics, Markdown and text.

#include <QGuiApplication>
#include <QDir>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <cstdio>

#include "backend.h"
#include "cli.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Commands and help draw no window, so they must not need a display, even
    // where the desktop exports QT_QPA_PLATFORM=wayland: QGuiApplication would
    // abort before seeing its arguments. Bare omaread prints help, as a
    // command line tool should; launchers say omaread open.
    const QByteArray first = argc > 1 ? QByteArray(argv[1]) : QByteArray();
    const bool command = argc == 1 || isCliCommand(first) || first == "--help" || first == "-h" ||
                         first == "--version" || first == "-v";
    if (command) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        // Qt's GTK platform theme opens a display of its own.
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("omaread");
    app.setApplicationVersion(OMAREAD_VERSION);

    if (first == "--version" || first == "-v") {
        printf("omaread %s\n", qPrintable(app.applicationVersion()));
        return 0;
    }
    // The commands' output is read by scripts and agents, so it stays English.
    if (command)
        return runCli(QStringList{"omaread"} + (first == "--help" || first == "-h" ? QStringList{"help"}
                                                                                  : app.arguments().mid(1)));
    QStringList arguments = app.arguments().mid(1);
    if (first == "open")
        arguments.removeFirst();

    // Matches the window to omaread.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("omaread");
    app.setWindowIcon(QIcon::fromTheme("omaread", QIcon(":/omaread.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    QObject::connect(&translator, &Translator::changed, &backend, &Backend::retranslate);
    // Ink images follow the theme's text color as the theme changes.
    backend.setInk(theme.foreground());
    QObject::connect(&theme, &Theme::changed, &backend, [&] { backend.setInk(theme.foreground()); });

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.addImageProvider("omaread", backend.createPageProvider());
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    // `omaread open book.epub` opens it, as the launcher does with a file.
    if (!arguments.isEmpty())
        backend.openUrl(QUrl::fromUserInput(arguments.first(), QDir::currentPath(), QUrl::AssumeLocalFile));

    // `omarchy agent look app` runs the app offscreen with these set, to see
    // the window without a display: size it to OMARCHY_LOOK_SIZE when given,
    // press the keys in OMARCHY_LOOK_KEYS through the window's Shortcuts,
    // save it once it has settled, and quit.
    if (const QString shot = qEnvironmentVariable("OMARCHY_LOOK_SCREENSHOT"); !shot.isEmpty()) {
        const QStringList size = qEnvironmentVariable("OMARCHY_LOOK_SIZE").split('x');
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()); window && size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        const QStringList keys = qEnvironmentVariable("OMARCHY_LOOK_KEYS").split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < keys.size(); ++i) {
            QTimer::singleShot(800 + i * 300, &app, [&engine, name = keys[i]] {
                auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
                const QKeySequence key(name);
                for (QObject *shortcut : window->findChildren<QObject *>()) {
                    if (qstrcmp(shortcut->metaObject()->className(), "QQuickShortcut") != 0
                        || !shortcut->property("enabled").toBool())
                        continue;
                    QVariantList bound = shortcut->property("sequences").toList();
                    bound.append(shortcut->property("sequence"));
                    for (const QVariant &sequence : bound) {
                        const QKeySequence match = sequence.typeId() == QMetaType::Int
                            ? QKeySequence(QKeySequence::StandardKey(sequence.toInt()))
                            : QKeySequence(sequence.toString());
                        if (!match.isEmpty() && match == key) {
                            QMetaObject::invokeMethod(shortcut, "activated");
                            return;
                        }
                    }
                }
                // Not a shortcut: typed into whatever has the keyboard, such as a search field.
                if (key.isEmpty())
                    return;
                const QKeyCombination combination = key[0];
                const QString text = name.size() == 1 ? name : QString();
                QKeyEvent press(QEvent::KeyPress, combination.key(), combination.keyboardModifiers(), text);
                QKeyEvent release(QEvent::KeyRelease, combination.key(), combination.keyboardModifiers(), text);
                QCoreApplication::sendEvent(window, &press);
                QCoreApplication::sendEvent(window, &release);
            });
        }
        const int delay = qEnvironmentVariableIsSet("OMARCHY_LOOK_DELAY_MS") ? qEnvironmentVariableIntValue("OMARCHY_LOOK_DELAY_MS") : 1500;
        QTimer::singleShot(800 + keys.size() * 300 + delay, &app, [&engine, shot] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()))
                window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
