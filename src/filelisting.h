#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Directory listings for the built-in file dialog. The names are LocalPath strings, so that names which
// are not valid UTF-8 can be listed and chosen exactly (Qt's and KDE's file dialogs replace their bytes).
namespace FileListing {

struct Entry
{
    QString name;          // LocalPath string
    bool isDir = false;    // a directory or a symlink to one
    bool isSymLink = false;
    qint64 size = 0;       // of the file, or of the link target
    qint64 modified = 0;   // seconds since the epoch
};

// All entries of `dir` except "." and "..", in the order of readdir(). `*error` receives errno
// (0 when the directory could be read).
QList<Entry> list(const QString &dir, int *error = nullptr);

// The path for text typed in the form of LocalPath::display(): "\xNN" with NN >= 80 becomes the byte.
QString fromDisplay(const QString &text);
// The bytes of `name` that are not valid UTF-8, e.g. "E9 FC" (empty for a plain name).
QString invalidBytes(const QString &name);
// `name` with those bytes read as Latin-1, a hint for names from old systems: "caf\xe9" -> "café".
QString latin1Reading(const QString &name);

// The mount points in the contents of /proc/self/mounts, with escapes such as "\040" (space) decoded.
QStringList mountPoints(const QByteArray &mounts);

} // namespace FileListing
