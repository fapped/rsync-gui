#pragma once

#include <QByteArray>
#include <QString>

// Lossless local file names. Linux file names are bytes; Qt converts them to and from UTF-8, which
// destroys names that are not valid UTF-8 (e.g. "\xff" becomes U+FFFD). Here every byte that is not
// part of a valid UTF-8 sequence is kept as a private-use character U+F780 + (byte - 0x80), the same
// idea as Python's "surrogateescape": decode() and encode() are exact inverses for any bytes.
// (A valid sequence that itself encodes U+F780..U+F7FF is escaped byte by byte, so that it survives too.)
// Paths in Location, the source list and the settings use this representation; they are encoded
// back to bytes only for system calls and for the rsync command line.
namespace LocalPath {

QString decode(const QByteArray &bytes);
QByteArray encode(const QString &path);
// Whether the path consists of valid UTF-8 only (no escaped bytes).
bool isPlain(const QString &path);
// For showing to the user: escaped bytes are written as "\xff".
QString display(const QString &path);
// "file:///a/%FF" -> the path with the exact bytes (QUrl::toLocalFile() would replace them).
QString fromFileUrl(const QString &url);
QString fromUrlPath(const QString &encodedPath);

struct Info
{
    bool exists = false;  // also a dangling symlink
    bool isSymLink = false;
    bool isDir = false;   // a real directory, not a symlink to one
    bool isFile = false;  // a regular file, not a symlink
    qint64 size = 0;
    QString symLinkTarget; // as stored in the link
};
Info info(const QString &path); // lstat(), symlinks are not followed
// stat(): a directory or a symlink to one.
bool isDirectory(const QString &path);

// realpath(); the cleaned path if it cannot be resolved (e.g. it does not exist).
QString canonical(const QString &path);
// mkdir -p; returns false if the directory could not be created.
bool makePath(const QString &path);

} // namespace LocalPath
