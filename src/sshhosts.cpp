#include "sshhosts.h"
#include "localpath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace SshHosts {

SshIdentity resolve(const QStringList &sshCommand, const QString &target, int port)
{
    if (sshCommand.isEmpty() || QFileInfo(sshCommand.first()).fileName() != QStringLiteral("ssh") || target.isEmpty())
        return {};
    QStringList args = sshCommand.mid(1);
    if (port > 0)
        args << QStringLiteral("-p") << QString::number(port);
    args << QStringLiteral("-G") << QStringLiteral("--") << target;
    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(sshCommand.first(), args);
    // -G only reads the configuration (no connection), it is quick.
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        return {};
    }
    SshIdentity identity;
    for (const QByteArray &line : process.readAllStandardOutput().split('\n')) {
        const qsizetype space = line.indexOf(' ');
        if (space <= 0)
            continue;
        const QByteArray key = line.left(space);
        const QString value = QString::fromLocal8Bit(line.mid(space + 1)).trimmed();
        if (key == "user")
            identity.user = value;
        else if (key == "hostname")
            identity.hostname = value;
        else if (key == "port")
            identity.port = value.toInt();
    }
    return identity;
}

QStringList unify(QList<Location> &sources, Location &destination, const Resolver &resolver)
{
    QStringList notes;
    QList<std::pair<SshIdentity, Location>> seen; // an identity and the first spelling of it
    QHash<QString, SshIdentity> identities;       // by originKey
    const auto process = [&](Location &location) {
        if (!location.isRemote())
            return;
        const QString key = location.originKey();
        if (!identities.contains(key))
            identities.insert(key, resolver(location));
        const SshIdentity identity = identities.value(key);
        if (!identity.isValid())
            return;
        for (const auto &[known, first] : std::as_const(seen)) {
            if (known == identity) {
                if (first.originKey() != key) {
                    const QString before = location.toString();
                    location.user = first.user;
                    location.host = first.host;
                    location.port = first.port;
                    const QString name = (first.user.isEmpty() ? first.host : first.user + QLatin1Char('@') + first.host)
                        + (first.port > 0 ? QStringLiteral(":%1").arg(first.port) : QString());
                    notes << QObject::tr("%1 is on the same server as %2 (ssh: %3@%4) - used as %2")
                                 .arg(LocalPath::display(before), name, identity.user, identity.hostname);
                }
                return;
            }
        }
        seen.append({identity, location});
    };
    for (Location &source : sources)
        process(source);
    process(destination);
    return notes;
}

QStringList configAliases(const QString &configFile)
{
    QFile file(configFile);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList aliases;
    for (const QByteArray &raw : file.readAll().split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        // "Host a b", "Host=a" (keywords are case-insensitive)
        if (!line.startsWith(QStringLiteral("host"), Qt::CaseInsensitive) || line.size() < 5
            || !(line.at(4).isSpace() || line.at(4) == QLatin1Char('=')))
            continue;
        for (const QString &pattern : line.mid(5).split(QRegularExpression(QStringLiteral("[\\s=]+")), Qt::SkipEmptyParts)) {
            if (!pattern.contains(QLatin1Char('*')) && !pattern.contains(QLatin1Char('?'))
                && !pattern.startsWith(QLatin1Char('!')) && !aliases.contains(pattern))
                aliases << pattern;
        }
    }
    return aliases;
}

QString configFile(const QStringList &sshCommand)
{
    for (qsizetype i = 1; i < sshCommand.size(); ++i) {
        if (sshCommand.at(i) == QStringLiteral("-F") && i + 1 < sshCommand.size())
            return sshCommand.at(i + 1);
        if (sshCommand.at(i).startsWith(QStringLiteral("-F")) && sshCommand.at(i).size() > 2)
            return sshCommand.at(i).mid(2);
    }
    return QDir::homePath() + QStringLiteral("/.ssh/config");
}

QString aliasFor(const QStringList &sshCommand, const QString &server, const QString &user)
{
    if (server.isEmpty())
        return {};
    for (const QString &alias : configAliases(configFile(sshCommand))) {
        const SshIdentity identity = resolve(sshCommand, alias, 0);
        if (identity.isValid() && identity.hostname.compare(server, Qt::CaseInsensitive) == 0
            && (user.isEmpty() || identity.user == user))
            return alias;
    }
    return {};
}

QString suggestTarget(const QStringList &sshCommand, const QString &server, const QString &user,
                      const QList<Location> &candidates, const Resolver &resolver)
{
    for (const Location &location : candidates) {
        if (!location.isRemote())
            continue;
        const SshIdentity identity = resolver(location);
        if (identity.isValid() && identity.hostname.compare(server, Qt::CaseInsensitive) == 0
            && (user.isEmpty() || identity.user == user))
            return location.user.isEmpty() ? location.host : location.user + QLatin1Char('@') + location.host;
    }
    // The user of a CIFS mount is Samba's, which need not be the ssh user: an alias of the server with any user next.
    if (const QString alias = aliasFor(sshCommand, server, user); !alias.isEmpty())
        return alias;
    if (const QString alias = aliasFor(sshCommand, server, QString()); !user.isEmpty() && !alias.isEmpty())
        return alias;
    return user.isEmpty() ? server : user + QLatin1Char('@') + server;
}

} // namespace SshHosts
