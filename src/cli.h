#pragma once

#include <QString>
#include <QStringList>

// The commands that need no display, for scripts and coding agents: they read
// a book and print what is in it, and never change it.
bool isCliCommand(const QString &word);
QString cliSummary();
// arguments[0] is the program, arguments[1] the command. Returns the exit code.
int runCli(const QStringList &arguments);
