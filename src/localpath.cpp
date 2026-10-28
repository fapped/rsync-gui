#include "localpath.h"

#include <QDir>
#include <QUrl>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr char16_t kEscapeFirst = 0xF780; // byte 0x80
constexpr char16_t kEscapeLast = 0xF7FF;  // byte 0xFF

bool isEscape(QChar c)
{
    return c.unicode() >= kEscapeFirst && c.unicode() <= kEscapeLast;
}

bool isContinuation(uchar b, uchar low = 0x80, uchar high = 0xBF)
{
    return b >= low && b <= high;
}

// Length of the valid UTF-8 sequence at `i` (strict: no overlong forms, surrogates or values
// above U+10FFFF), or 0 if the byte at `i` does not start one.
int sequenceLength(const QByteArray &bytes, qsizetype i)
{
    const qsizetype n = bytes.size();
    const uchar b0 = uchar(bytes.at(i));
    const auto at = [&](qsizetype k) { return k < n ? uchar(bytes.at(k)) : uchar(0); };
    if (b0 < 0x80)
        return 1;
    if (b0 >= 0xC2 && b0 <= 0xDF)
        return isContinuation(at(i + 1)) ? 2 : 0;
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        const uchar low = b0 == 0xE0 ? 0xA0 : 0x80;
        const uchar high = b0 == 0xED ? 0x9F : 0xBF;
        if (!isContinuation(at(i + 1), low, high) || !isContinuation(at(i + 2)))
            return 0;
        // U+F780..U+F7FF (EF 9E xx, EF 9F xx) are the escape characters themselves.
        if (b0 == 0xEF && (at(i + 1) == 0x9E || at(i + 1) == 0x9F))
            return 0;
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        const uchar low = b0 == 0xF0 ? 0x90 : 0x80;
        const uchar high = b0 == 0xF4 ? 0x8F : 0xBF;
        return isContinuation(at(i + 1), low, high) && isContinuation(at(i + 2)) && isContinuation(at(i + 3)) ? 4 : 0;
    }
    return 0;
}

} // namespace

namespace LocalPath {

QString decode(const QByteArray &bytes)
{
    QString result;
    result.reserve(bytes.size());
    qsizetype validStart = 0;
    qsizetype i = 0;
    while (i < bytes.size()) {
        const int length = sequenceLength(bytes, i);
        if (length > 0) {
            i += length;
            continue;
        }
        result += QString::fromUtf8(bytes.constData() + validStart, i - validStart);
        result += QChar(char16_t(kEscapeFirst + (uchar(bytes.at(i)) - 0x80)));
        validStart = ++i;
    }
    result += QString::fromUtf8(bytes.constData() + validStart, i - validStart);
    return result;
}

QByteArray encode(const QString &path)
{
    QByteArray result;
    result.reserve(path.size());
    qsizetype plainStart = 0;
    for (qsizetype i = 0; i < path.size(); ++i) {
        if (!isEscape(path.at(i)))
            continue;
        result += QStringView(path).mid(plainStart, i - plainStart).toUtf8();
        result += char(0x80 + (path.at(i).unicode() - kEscapeFirst));
        plainStart = i + 1;
    }
    result += QStringView(path).mid(plainStart).toUtf8();
    return result;
}

bool isPlain(const QString &path)
{
    for (const QChar c : path) {
        if (isEscape(c))
            return false;
    }
    return true;
}

QString display(const QString &path)
{
    if (isPlain(path))
        return path;
    QString result;
    for (const QChar c : path) {
        if (isEscape(c))
            result += QStringLiteral("\\x%1").arg(0x80 + (c.unicode() - kEscapeFirst), 2, 16, QLatin1Char('0'));
        else
            result += c;
    }
    return result;
}

QString fromUrlPath(const QString &encodedPath)
{
    return decode(QByteArray::fromPercentEncoding(encodedPath.toUtf8()));
}

QString fromFileUrl(const QString &url)
{
    return fromUrlPath(QUrl(url).path(QUrl::FullyEncoded));
}

Info info(const QString &path)
{
    Info result;
    const QByteArray bytes = encode(path);
    struct stat st;
    if (bytes.isEmpty() || ::lstat(bytes.constData(), &st) != 0)
        return result;
    result.exists = true;
    result.isSymLink = S_ISLNK(st.st_mode);
    result.isDir = S_ISDIR(st.st_mode);
    result.isFile = S_ISREG(st.st_mode);
    result.size = st.st_size;
    if (result.isSymLink) {
        QByteArray target(PATH_MAX, '\0');
        const ssize_t length = ::readlink(bytes.constData(), target.data(), target.size());
        if (length > 0)
            result.symLinkTarget = decode(target.left(length));
    }
    return result;
}

bool isDirectory(const QString &path)
{
    struct stat st;
    return !path.isEmpty() && ::stat(encode(path).constData(), &st) == 0 && S_ISDIR(st.st_mode);
}

QString canonical(const QString &path)
{
    if (char *resolved = ::realpath(encode(path).constData(), nullptr)) {
        const QString result = decode(QByteArray(resolved));
        std::free(resolved);
        return result;
    }
    return QDir::cleanPath(path);
}

bool makePath(const QString &path)
{
    const QByteArray bytes = encode(QDir::cleanPath(path));
    if (bytes.isEmpty())
        return false;
    // Create each missing component in turn, like mkdir -p.
    for (qsizetype slash = bytes.indexOf('/', 1); ; slash = bytes.indexOf('/', slash + 1)) {
        const QByteArray part = slash < 0 ? bytes : bytes.left(slash);
        if (::mkdir(part.constData(), 0777) != 0 && errno != EEXIST)
            return false;
        if (slash < 0)
            break;
    }
    return isDirectory(path); // a symlink to a directory is fine, like for mkdir -p
}

} // namespace LocalPath
