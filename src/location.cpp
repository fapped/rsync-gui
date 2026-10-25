#include "location.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

namespace {

QString stripQuotes(const QString &text)
{
    QString s = text.trimmed();
    if (s.size() >= 2
        && ((s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
            || (s.startsWith(QLatin1Char('\'')) && s.endsWith(QLatin1Char('\''))))) {
        s = s.mid(1, s.size() - 2);
    }
    return s;
}

QString cleanRemotePath(const QString &path)
{
    // QDir::cleanPath would turn an empty path (the remote home directory) into ".".
    if (path.isEmpty())
        return QStringLiteral(".");
    const bool trailingSlash = path.size() > 1 && path.endsWith(QLatin1Char('/'));
    QString cleaned = QDir::cleanPath(path);
    // A trailing slash on a remote source matters to rsync ("copy the contents"), keep it.
    if (trailingSlash && cleaned != QStringLiteral("/"))
        cleaned += QLatin1Char('/');
    return cleaned;
}

} // namespace

Location Location::parse(const QString &text)
{
    Location loc;
    QString s = stripQuotes(text);
    if (s.isEmpty())
        return loc;

    if (s.startsWith(QStringLiteral("file://"))) {
        s = QUrl(s).toLocalFile();
    } else if (s.startsWith(QStringLiteral("sftp://")) || s.startsWith(QStringLiteral("ssh://"))) {
        const QUrl url(s);
        if (!url.isValid() || url.host().isEmpty())
            return loc;
        loc.user = url.userName();
        loc.host = url.host();
        loc.port = qMax(0, url.port(0));
        // Dolphin uses "sftp://host/~/dir" for paths relative to the home directory.
        QString path = url.path(QUrl::FullyDecoded);
        if (path.startsWith(QStringLiteral("/~/")))
            path = path.mid(3);
        else if (path == QStringLiteral("/~"))
            path.clear();
        else if (path.isEmpty())
            path = QStringLiteral("/");
        loc.path = cleanRemotePath(path);
        return loc;
    } else if (!s.startsWith(QLatin1Char('/')) && !s.startsWith(QLatin1Char('~')) && !s.startsWith(QLatin1Char('.'))) {
        // rsync's remote syntax: a colon before the first slash ("host:path", "user@host:/path").
        static const QRegularExpression remote(QStringLiteral("^(?:([^@/:]+)@)?([^@/:]+):(?!:)(.*)$"));
        const QRegularExpressionMatch m = remote.match(s);
        if (m.hasMatch()) {
            loc.user = m.captured(1);
            loc.host = m.captured(2);
            loc.path = cleanRemotePath(m.captured(3));
            return loc;
        }
    }

    if (s == QStringLiteral("~"))
        s = QDir::homePath();
    else if (s.startsWith(QStringLiteral("~/")))
        s = QDir::homePath() + s.mid(1);
    if (s.isEmpty())
        return loc;
    loc.path = QDir::cleanPath(QFileInfo(s).absoluteFilePath());
    return loc;
}

QString Location::rsyncSpec() const
{
    if (!isRemote())
        return path;
    const QString userPart = user.isEmpty() ? QString() : user + QLatin1Char('@');
    return userPart + host + QLatin1Char(':') + path;
}

QString Location::toString() const
{
    if (!isRemote() || port == 0)
        return rsyncSpec();
    QUrl url;
    url.setScheme(QStringLiteral("sftp"));
    url.setUserName(user);
    url.setHost(host);
    url.setPort(port);
    url.setPath(path.startsWith(QLatin1Char('/')) ? path : QStringLiteral("/~/") + path);
    return url.toString();
}

QString Location::originKey() const
{
    if (!isRemote())
        return {};
    return user + QLatin1Char('@') + host + QLatin1Char(':') + QString::number(port);
}

bool Location::operator==(const Location &other) const
{
    return path == other.path && user == other.user && host == other.host && port == other.port;
}
