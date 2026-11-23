#include "cli.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

#include "backend.h"
#include "book.h"

namespace {

void print(FILE *stream, const QString &text) { fprintf(stream, "%s\n", qPrintable(text)); }
void print(const QJsonObject &object) { fputs(QJsonDocument(object).toJson().constData(), stdout); }
int fail(const QString &message) {
    print(stderr, message);
    return 1;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
    }
    void book() { parser.addPositionalArgument("file", "A PDF, EPUB, comic, Markdown or text file", "<file>"); }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    void password() { parser.addOption({"password", "The password of a locked PDF", "password"}); }
    bool isJson() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
};

// The book the command names, read the way the window reads it, or null after
// saying on stderr why it can't be.
std::shared_ptr<Book> open(const Command &command) {
    const QString path = command.argument(1);
    if (path.isEmpty()) {
        print(stderr, "Name a book or document.");
        return nullptr;
    }
    Book::Error error = Book::Error::None;
    auto book = Book::load(path, 1, &error, command.parser.value("password"));
    switch (error) {
    case Book::Error::None: break;
    case Book::Error::Missing: print(stderr, "Can't find " + path + "."); break;
    case Book::Error::Unsupported:
        print(stderr, path + " isn't a PDF, EPUB, comic archive, Markdown or text file.");
        break;
    case Book::Error::Locked: print(stderr, path + " is locked; pass --password."); break;
    case Book::Error::WrongPassword: print(stderr, "That password doesn't open " + path + "."); break;
    case Book::Error::Empty: print(stderr, path + " has nothing in it to read."); break;
    case Book::Error::Unreadable: print(stderr, path + " couldn't be read. It may be damaged."); break;
    }
    return book;
}

// Sections are numbered from 1 on the command line, as the window shows them.
QJsonArray contents(const Book &book) {
    QJsonArray entries;
    for (const TocEntry &entry : book.toc)
        entries.append(QJsonObject{{"title", entry.title}, {"section", entry.section + 1}, {"depth", entry.depth}});
    return entries;
}

int info(const QStringList &arguments) {
    Command command("info", "Print a book's title, author, format, sections and table of contents.");
    command.book();
    command.json();
    command.password();
    command.parser.process(arguments);
    const auto book = open(command);
    if (!book)
        return 1;
    if (command.isJson()) {
        print(QJsonObject{{"path", book->path},
                          {"title", book->title},
                          {"author", book->author},
                          {"format", book->kindName()},
                          {"paged", book->paged()},
                          {"sections", book->sectionCount()},
                          {"contents", contents(*book)}});
        return 0;
    }
    print(stdout, "Title:    " + book->title);
    if (!book->author.isEmpty())
        print(stdout, "Author:   " + book->author);
    print(stdout, "Format:   " + book->kindName());
    print(stdout, QString(book->paged() ? "Pages:    %1" : "Sections: %1").arg(book->sectionCount()));
    if (!book->toc.isEmpty()) {
        print(stdout, "\nContents:");
        for (const TocEntry &entry : book->toc)
            print(stdout, QString("  %1%2  (%3)").arg(QString(entry.depth * 2, ' '), entry.title).arg(entry.section + 1));
    }
    return 0;
}

int text(const QStringList &arguments) {
    Command command("text", "Print a book's words, or one section's with --section.");
    command.book();
    command.json();
    command.password();
    command.parser.addOption({"section", "Only this section or page, counting from 1", "n"});
    command.parser.process(arguments);
    const auto book = open(command);
    if (!book)
        return 1;
    int first = 0, last = book->sectionCount() - 1;
    if (command.parser.isSet("section")) {
        bool ok = false;
        const int section = command.parser.value("section").toInt(&ok);
        if (!ok || section < 1 || section > book->sectionCount())
            return fail(QString("Choose a section from 1 to %1.").arg(book->sectionCount()));
        first = last = section - 1;
    }
    if (book->kind == Book::Kind::Comic)
        return fail(book->path + " is a comic; its pages are pictures, with no text to print.");
    QJsonArray sections;
    for (int section = first; section <= last; ++section) {
        const QString words = book->plainText(section);
        if (command.isJson())
            sections.append(QJsonObject{{"section", section + 1}, {"text", words}});
        else
            fputs(qPrintable(words + (words.endsWith('\n') ? "" : "\n") + (section < last ? "\n" : "")), stdout);
    }
    if (command.isJson())
        print(QJsonObject{{"title", book->title}, {"sections", sections}});
    return 0;
}

int search(const QStringList &arguments) {
    Command command("search", "Find words in a book, ignoring case: each section or page they appear in, and how often.");
    command.book();
    command.parser.addPositionalArgument("words", "What to look for", "<words>");
    command.json();
    command.password();
    command.parser.process(arguments);
    const QString words = command.parser.positionalArguments().mid(2).join(' ').simplified();
    if (words.isEmpty())
        return fail("Say what to look for.");
    const auto book = open(command);
    if (!book)
        return 1;
    QMap<int, int> counts;
    for (const SearchHit &hit : book->search(words))
        ++counts[hit.section];
    if (command.isJson()) {
        QJsonArray found;
        int total = 0;
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            found.append(QJsonObject{{"section", it.key() + 1}, {"matches", it.value()}});
            total += it.value();
        }
        print(QJsonObject{{"words", words}, {"matches", total}, {"sections", found}});
        return 0;
    }
    if (counts.isEmpty()) {
        print(stdout, "No matches for \"" + words + "\".");
        return 0;
    }
    for (auto it = counts.begin(); it != counts.end(); ++it)
        print(stdout, QString("%1 %2: %3").arg(book->paged() ? "Page" : "Section").arg(it.key() + 1).arg(it.value()));
    return 0;
}

// The reader's highlights and notes in a book, as the library keeps them:
// Markdown to keep or share, or JSON.
int highlights(const QStringList &arguments) {
    Command command("highlights", "Print the passages highlighted in a book, and the notes on them, as Markdown.");
    command.book();
    command.json();
    command.password();
    command.parser.process(arguments);
    const auto book = open(command);
    if (!book)
        return 1;
    QList<Highlight> marks;
    QFile state(Backend::stateFile());
    if (state.open(QIODevice::ReadOnly)) {
        const QJsonArray books = QJsonDocument::fromJson(state.readAll()).object().value("books").toArray();
        for (const QJsonValue &value : books) {
            const QJsonObject entry = value.toObject();
            if (entry.value("path").toString() != book->path)
                continue;
            for (const QJsonValue &mark : entry.value("highlights").toArray()) {
                const QJsonObject m = mark.toObject();
                marks << Highlight{m.value("section").toInt(), m.value("start").toInt(), m.value("end").toInt(),
                                   m.value("text").toString(), m.value("note").toString(), m.value("color").toString()};
            }
        }
    }
    if (command.isJson()) {
        QJsonArray list;
        for (const Highlight &h : std::as_const(marks))
            list.append(QJsonObject{{"section", h.section + 1}, {"chapter", book->chapterLabel(h.section)},
                                    {"text", h.text},           {"note", h.note},
                                    {"color", h.color}});
        print(QJsonObject{{"title", book->title}, {"author", book->author}, {"highlights", list}});
        return 0;
    }
    if (marks.isEmpty()) {
        print(stdout, "No highlights in " + book->title + " yet.");
        return 0;
    }
    fputs(book->notesMarkdown(marks).toUtf8().constData(), stdout);
    return 0;
}

// As Hype and Signed do: one shared copy, linked into Claude Code, found
// directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.");
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/omaread and link into ~/.claude/skills",
                                         "[install]");
    command.json();
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return 1;
    const QByteArray text = bundled.readAll();
    if (command.argument(1).isEmpty()) {
        if (command.isJson())
            print({{"skill", QString::fromUtf8(text)}});
        else
            fputs(text.constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use omaread skill to print the skill, or omaread skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/omaread");
    QSaveFile file(directory + "/SKILL.md");
    file.setDirectWriteFallback(false);
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    QJsonObject result{{"installed", file.fileName()}};
    auto done = [&] {
        if (command.isJson())
            print(result);
        return 0;
    };
    if (!command.isJson())
        print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return done();
    const QString link = home.filePath(".claude/skills/omaread");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/omaread", link))
        return fail("Could not link " + link);
    result.insert("linked", link);
    if (!command.isJson())
        print(stdout, "Linked " + link);
    return done();
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: omaread <command> [options]\n\n" + cliSummary());
    return 0;
}

} // namespace

QString cliSummary() {
    return "App:\n"
           "  open [file]                  Open Omaread, on a book if one is named\n\n"
           "Commands that need no display (omaread help <command> for options):\n"
           "  info <file>                  Title, author, format, sections and contents\n"
           "  text <file>                  The words, of the whole book or one section\n"
           "  search <file> <words>        Where words appear, section by section\n"
           "  highlights <file>            The passages highlighted, and the notes on them\n"
           "  skill [install]              Print the skill for coding agents, or install it\n\n"
           "Each command takes --json, and --password for a locked PDF.";
}

bool isCliCommand(const QString &word) {
    return QStringList{"info", "text", "search", "highlights", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "info"     ? info(arguments)
           : command == "text"   ? text(arguments)
           : command == "search" ? search(arguments)
           : command == "highlights" ? highlights(arguments)
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
