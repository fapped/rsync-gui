#include "filelisting.h"

#include "localpath.h"

#include <QRegularExpression>

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

namespace {

// The byte that an escape character of LocalPath stands for, or -1 for an ordinary character.
int escapedByte(QChar c)
{
    const QString single(c);
    if (LocalPath::isPlain(single))
        return -1;
    return uchar(LocalPath::encode(single).at(0));
}

// A name with U+FFFD in it, as a pattern: each U+FFFD stands for one or more escaped bytes.
QRegularExpression replacedBytesPattern(const QString &name)
{
    const QStringList parts = name.split(QChar::ReplacementCharacter);
    QString pattern = QRegularExpression::escape(parts.first());
    for (qsizetype i = 1; i < parts.size(); ++i)
        pattern += QStringLiteral("(?:[\\x{F780}-\\x{F7FF}]+|\\x{FFFD})") + QRegularExpression::escape(parts.at(i));
    return QRegularExpression(QRegularExpression::anchoredPattern(pattern));
}

} // namespace

namespace FileListing {

QList<Entry> list(const QString &dir, int *error)
{
    QList<Entry> entries;
    QByteArray base = LocalPath::encode(dir);
    if (!base.endsWith('/'))
        base += '/';
    DIR *d = ::opendir(base.constData());
    if (!d) {
        if (error)
            *error = errno;
        return entries;
    }
    errno = 0;
    while (const dirent *e = ::readdir(d)) {
        const QByteArray name(e->d_name);
        if (name == "." || name == "..")
            continue;
        const QByteArray path = base + name;
        struct stat st;
        if (::lstat(path.constData(), &st) != 0)
            continue; // removed in the meantime
        Entry entry;
        entry.name = LocalPath::decode(name);
        entry.isSymLink = S_ISLNK(st.st_mode);
        struct stat target;
        if (entry.isSymLink && ::stat(path.constData(), &target) == 0)
            st = target;
        entry.isDir = S_ISDIR(st.st_mode);
        entry.size = st.st_size;
        entry.modified = st.st_mtime;
        entries << entry;
    }
    ::closedir(d);
    if (error)
        *error = 0;
    return entries;
}

QString fromDisplay(const QString &text)
{
    // Back to bytes and decoded again, so that the result is the same string as a listing gives
    // (e.g. "\xc3\xa9" is "é", which is valid UTF-8).
    static const QRegularExpression escape(QStringLiteral("\\\\x([89a-fA-F][0-9a-fA-F])"));
    if (!text.contains(QLatin1String("\\x")))
        return text;
    QByteArray bytes;
    qsizetype last = 0;
    for (auto it = escape.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        bytes += LocalPath::encode(text.mid(last, m.capturedStart() - last));
        bytes += char(m.captured(1).toInt(nullptr, 16));
        last = m.capturedEnd();
    }
    return LocalPath::decode(bytes + LocalPath::encode(text.mid(last)));
}

QString invalidBytes(const QString &name)
{
    QStringList bytes;
    for (const QChar c : name) {
        if (const int byte = escapedByte(c); byte >= 0)
            bytes << QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
    }
    return bytes.join(QLatin1Char(' '));
}

QString latin1Reading(const QString &name)
{
    QString result;
    for (const QChar c : name) {
        const int byte = escapedByte(c);
        result += byte >= 0 ? QChar(char16_t(byte)) : c;
    }
    return result;
}

QStringList replacedBytesMatches(const QString &path, int limit)
{
    if (!path.startsWith(QLatin1Char('/')) || !path.contains(QChar::ReplacementCharacter))
        return {};
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList candidates{QString()}; // the paths so far, without the trailing slash
    for (qsizetype i = 0; i < parts.size() && !candidates.isEmpty(); ++i) {
        const QString &part = parts.at(i);
        const bool last = i == parts.size() - 1;
        QStringList next;
        for (const QString &base : std::as_const(candidates)) {
            if (!part.contains(QChar::ReplacementCharacter)) {
                next << base + QLatin1Char('/') + part;
                continue;
            }
            const QRegularExpression pattern = replacedBytesPattern(part);
            for (const Entry &entry : list(base.isEmpty() ? QStringLiteral("/") : base)) {
                if ((last || entry.isDir) && pattern.match(entry.name).hasMatch() && next.size() < limit)
                    next << base + QLatin1Char('/') + entry.name;
            }
        }
        candidates = next;
    }
    QStringList matches;
    for (const QString &candidate : std::as_const(candidates)) {
        if (LocalPath::info(candidate).exists)
            matches << candidate;
    }
    matches.sort();
    return matches;
}

QStringList mountPoints(const QByteArray &mounts)
{
    QStringList points;
    for (const QByteArray &line : mounts.split('\n')) {
        const QList<QByteArray> fields = line.split(' ');
        if (fields.size() < 2)
            continue;
        const QByteArray &field = fields.at(1);
        QByteArray point;
        for (qsizetype i = 0; i < field.size(); ++i) {
            bool octal = false;
            if (field.at(i) == '\\' && i + 3 < field.size()) {
                const int value = field.mid(i + 1, 3).toInt(&octal, 8);
                if (octal && value < 256) {
                    point += char(value);
                    i += 3;
                    continue;
                }
            }
            point += field.at(i);
        }
        points << LocalPath::decode(point);
    }
    return points;
}

} // namespace FileListing
