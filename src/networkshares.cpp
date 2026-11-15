#include "networkshares.h"
#include "localpath.h"
#include "rsyncoptions.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QUrl>

namespace {

// "\040" -> " " etc. (the escapes of /proc/self/mountinfo), as a path with the exact bytes (LocalPath).
QString unescape(const QByteArray &field)
{
    QByteArray bytes;
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field.at(i) == '\\' && i + 3 < field.size()) {
            const QByteArray octal = field.mid(i + 1, 3);
            bool ok = false;
            const int value = octal.toInt(&ok, 8);
            if (ok && octal.size() == 3) {
                bytes += char(value);
                i += 3;
                continue;
            }
        }
        bytes += field.at(i);
    }
    return LocalPath::decode(bytes);
}

QString option(const QString &options, const QString &name)
{
    for (const QString &item : options.split(QLatin1Char(','))) {
        if (item.startsWith(name + QLatin1Char('=')))
            return item.mid(name.size() + 1);
    }
    return {};
}

QString joinPath(const QString &base, const QString &relative)
{
    if (relative.isEmpty())
        return base;
    if (base.isEmpty())
        return relative;
    return base.endsWith(QLatin1Char('/')) ? base + relative : base + QLatin1Char('/') + relative;
}

QString useName(ShareSettings::Use use)
{
    switch (use) {
    case ShareSettings::Use::Enabled: return QStringLiteral("enabled");
    case ShareSettings::Use::Disabled: return QStringLiteral("disabled");
    case ShareSettings::Use::Never: return QStringLiteral("never");
    case ShareSettings::Use::Unset: break;
    }
    return QStringLiteral("unset");
}

ShareSettings::Use useFromName(const QString &name)
{
    if (name == QLatin1String("enabled"))
        return ShareSettings::Use::Enabled;
    if (name == QLatin1String("disabled"))
        return ShareSettings::Use::Disabled;
    if (name == QLatin1String("never"))
        return ShareSettings::Use::Never;
    return ShareSettings::Use::Unset;
}

QString groupFor(const QString &key)
{
    // QSettings groups cannot contain "/" - the key is percent-encoded.
    return QStringLiteral("shares/") + QString::fromLatin1(QUrl::toPercentEncoding(key));
}

} // namespace

QList<Mount> Mount::parse(const QByteArray &mountinfo)
{
    QList<Mount> mounts;
    for (const QByteArray &line : mountinfo.split('\n')) {
        // "36 35 98:0 /root /mount/point rw,noatime master:1 - ext3 /dev/root rw,errors=continue"
        const QList<QByteArray> fields = line.split(' ');
        const qsizetype separator = fields.indexOf("-");
        if (separator < 6 || separator + 2 >= fields.size())
            continue;
        Mount mount;
        mount.root = unescape(fields.at(3));
        mount.mountPoint = unescape(fields.at(4));
        mount.fsType = QString::fromLatin1(fields.at(separator + 1));
        mount.source = unescape(fields.at(separator + 2));
        if (separator + 3 < fields.size())
            mount.superOptions = unescape(fields.at(separator + 3));
        mounts << mount;
    }
    return mounts;
}

QList<Mount> Mount::current()
{
    QFile file(QStringLiteral("/proc/self/mountinfo"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return parse(file.readAll());
}

std::optional<Mount> Mount::find(const QList<Mount> &mounts, const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    std::optional<Mount> best;
    for (const Mount &mount : mounts) {
        const QString &point = mount.mountPoint;
        const bool inside = clean == point || point == QStringLiteral("/")
            || clean.startsWith(point + QLatin1Char('/'));
        // The longest mount point wins; of equal ones the last (it hides the earlier ones).
        if (inside && (!best || point.size() >= best->mountPoint.size()))
            best = mount;
    }
    return best;
}

QString NetworkShare::display() const
{
    if (kind == Kind::Cifs)
        return QStringLiteral("//%1/%2").arg(server, share) + (remotePath.isEmpty() ? QString() : QLatin1Char('/') + remotePath);
    return (user.isEmpty() ? QString() : user + QLatin1Char('@')) + server + QLatin1Char(':') + remotePath;
}

std::optional<NetworkShare> NetworkShare::fromMount(const Mount &mount)
{
    NetworkShare share;
    share.mountPoint = mount.mountPoint;
    const QString root = mount.root == QStringLiteral("/") ? QString() : mount.root;
    if (mount.fsType == QStringLiteral("fuse.sshfs")) {
        // "[user@]host:path" (an IPv6 address in brackets)
        QString source = mount.source;
        const qsizetype at = source.indexOf(QLatin1Char('@'));
        if (at > 0 && at < source.indexOf(QLatin1Char(':'))) {
            share.user = source.left(at);
            source = source.mid(at + 1);
        }
        qsizetype colon = source.startsWith(QLatin1Char('[')) ? source.indexOf(QStringLiteral("]:")) + 1
                                                               : source.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            return std::nullopt;
        share.kind = Kind::Sshfs;
        share.server = source.left(colon);
        if (share.server.startsWith(QLatin1Char('[')) && share.server.endsWith(QLatin1Char(']')))
            share.server = share.server.mid(1, share.server.size() - 2);
        share.remotePath = joinPath(source.mid(colon + 1), root.mid(root.startsWith(QLatin1Char('/')) ? 1 : 0));
        share.key = QStringLiteral("sshfs:") + mount.source;
        return share;
    }
    if (mount.fsType == QStringLiteral("cifs") || mount.fsType == QStringLiteral("smb3")) {
        // "//server/share[/prefix]"; a prefix may also be in the root field or the prefixpath= option.
        if (!mount.source.startsWith(QStringLiteral("//")))
            return std::nullopt;
        const QStringList parts = mount.source.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() < 2)
            return std::nullopt;
        share.kind = Kind::Cifs;
        share.server = parts.at(0);
        share.share = parts.at(1);
        QString prefix = parts.mid(2).join(QLatin1Char('/'));
        if (prefix.isEmpty())
            prefix = option(mount.superOptions, QStringLiteral("prefixpath"));
        if (!root.isEmpty())
            prefix = joinPath(prefix, root.mid(1));
        while (prefix.startsWith(QLatin1Char('/')))
            prefix.remove(0, 1);
        share.remotePath = prefix;
        share.user = option(mount.superOptions, QStringLiteral("username"));
        share.key = QStringLiteral("//%1/%2").arg(share.server, share.share);
        return share;
    }
    return std::nullopt;
}

NetworkShare NetworkShare::fromKey(const QString &key)
{
    Mount mount;
    mount.root = QStringLiteral("/");
    if (key.startsWith(QStringLiteral("sshfs:"))) {
        mount.fsType = QStringLiteral("fuse.sshfs");
        mount.source = key.mid(6);
    } else {
        mount.fsType = QStringLiteral("cifs");
        mount.source = key;
    }
    NetworkShare share = fromMount(mount).value_or(NetworkShare());
    share.mountPoint.clear();
    share.key = key;
    return share;
}

std::optional<NetworkShare> NetworkShare::of(const QList<Mount> &mounts, const QString &localPath, QString *relative)
{
    const std::optional<Mount> mount = Mount::find(mounts, localPath);
    if (!mount)
        return std::nullopt;
    std::optional<NetworkShare> share = fromMount(*mount);
    if (share && relative) {
        const QString clean = QDir::cleanPath(localPath);
        *relative = clean.size() > mount->mountPoint.size() ? clean.mid(mount->mountPoint.size() + 1) : QString();
    }
    return share;
}

QString NetworkShare::sharePathCommand(const QString &share)
{
    return QStringLiteral("testparm -s --section-name=%1 --parameter-name=path 2>/dev/null").arg(remoteShellQuote(share));
}

QString NetworkShare::ownershipCommand(const QString &share)
{
    return QStringLiteral("for p in 'force user' 'force group' 'create mask' 'force create mode' 'directory mask' "
                          "'force directory mode'; do "
                          "printf '%s=%s\\n' \"$p\" \"$(testparm -s --section-name=%1 --parameter-name=\"$p\" "
                          "2>/dev/null)\"; done")
        .arg(remoteShellQuote(share));
}

QMap<QString, QString> NetworkShare::parseParameters(const QByteArray &output)
{
    QMap<QString, QString> parameters;
    for (const QString &line : QString::fromLocal8Bit(output).split(QLatin1Char('\n'))) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        const QString value = eq > 0 ? line.mid(eq + 1).trimmed() : QString();
        if (!value.isEmpty())
            parameters.insert(line.left(eq).trimmed(), value);
    }
    return parameters;
}

QString NetworkShare::chmodFromSamba(const QMap<QString, QString> &parameters)
{
    const auto octal = [&parameters](const char *name, int fallback) {
        bool ok = false;
        const int value = parameters.value(QLatin1String(name)).toInt(&ok, 8);
        return ok ? value & 0777 : fallback;
    };
    QStringList parts;
    const auto add = [&parts](QChar kind, int mask, int force) {
        static const char who[] = {'u', 'g', 'o'};
        for (int i = 0; i < 3; ++i) {
            const int shift = 6 - 3 * i;
            const auto bits = [](int value) {
                QString perms;
                if (value & 4)
                    perms += QLatin1Char('r');
                if (value & 2)
                    perms += QLatin1Char('w');
                if (value & 1)
                    perms += QLatin1Char('x');
                return perms;
            };
            // Removed first, then forced: Samba ORs the forced bits after the mask.
            if (const QString removed = bits((~mask >> shift) & 7); !removed.isEmpty())
                parts << kind + QLatin1Char(who[i]) + QLatin1Char('-') + removed;
            if (const QString added = bits((force >> shift) & 7); !added.isEmpty())
                parts << kind + QLatin1Char(who[i]) + QLatin1Char('+') + added;
        }
    };
    add(QLatin1Char('F'), octal("create mask", 0744), octal("force create mode", 0));
    add(QLatin1Char('D'), octal("directory mask", 0755), octal("force directory mode", 0));
    return parts.join(QLatin1Char(','));
}

Location ShareSettings::location(const QString &relative) const
{
    Location loc;
    const qsizetype at = sshTarget.lastIndexOf(QLatin1Char('@'));
    loc.user = at > 0 ? sshTarget.left(at) : QString();
    loc.host = at > 0 ? sshTarget.mid(at + 1) : sshTarget;
    loc.port = port;
    QString base = serverPath;
    while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
        base.chop(1);
    loc.path = joinPath(base, relative);
    return loc;
}

ShareSettings ShareSettings::detected(const NetworkShare &share)
{
    ShareSettings settings;
    settings.sshTarget = share.user.isEmpty() ? share.server : share.user + QLatin1Char('@') + share.server;
    if (share.kind == NetworkShare::Kind::Sshfs)
        settings.serverPath = share.remotePath.isEmpty() ? QStringLiteral("~") : share.remotePath;
    return settings;
}

ShareSettings ShareSettings::load(QSettings &settings, const QString &key)
{
    ShareSettings result;
    settings.beginGroup(groupFor(key));
    result.use = useFromName(settings.value(QStringLiteral("use")).toString());
    result.sshTarget = settings.value(QStringLiteral("sshTarget")).toString();
    result.port = settings.value(QStringLiteral("port"), 0).toInt();
    result.serverPath = settings.value(QStringLiteral("serverPath")).toString();
    result.chmod = settings.value(QStringLiteral("chmod")).toString();
    settings.endGroup();
    return result;
}

void ShareSettings::save(QSettings &settings, const QString &key) const
{
    settings.beginGroup(groupFor(key));
    settings.setValue(QStringLiteral("use"), useName(use));
    settings.setValue(QStringLiteral("sshTarget"), sshTarget);
    settings.setValue(QStringLiteral("port"), port);
    settings.setValue(QStringLiteral("serverPath"), serverPath);
    settings.setValue(QStringLiteral("chmod"), chmod);
    settings.endGroup();
}

QStringList ShareSettings::savedKeys(QSettings &settings)
{
    settings.beginGroup(QStringLiteral("shares"));
    const QStringList groups = settings.childGroups();
    settings.endGroup();
    QStringList keys;
    for (const QString &group : groups)
        keys << QUrl::fromPercentEncoding(group.toLatin1());
    return keys;
}

void ShareSettings::remove(QSettings &settings, const QString &key)
{
    settings.remove(groupFor(key));
}

ShareRouting ShareRouting::route(const QList<Location> &sources, const Location &destination, const QList<Mount> &mounts,
                                 const SettingsLookup &settings, bool useServer, bool contentsOnly)
{
    ShareRouting result;
    // A local location on a share set up for ssh -> its location on the server.
    const auto map = [&](const Location &loc, bool *mapped) -> Location {
        *mapped = false;
        if (loc.isRemote())
            return loc;
        QString relative;
        const std::optional<NetworkShare> share = NetworkShare::of(mounts, loc.path, &relative);
        if (!share || !useServer)
            return loc;
        const ShareSettings shareSettings = settings(share->key);
        if (shareSettings.use == ShareSettings::Use::Unset && !result.unsetKeys.contains(share->key)) {
            result.unsetKeys << share->key;
            result.unsetShares << *share;
        }
        if (!shareSettings.isUsable())
            return loc;
        *mapped = true;
        return shareSettings.location(relative);
    };
    const auto onNetworkMount = [&mounts](const Location &loc) {
        return !loc.isRemote() && NetworkShare::of(mounts, loc.path).has_value();
    };

    bool destMapped = false;
    result.destination = map(destination, &destMapped);
    if (destMapped) {
        if (const std::optional<NetworkShare> share = NetworkShare::of(mounts, destination.path))
            result.destinationChmod = settings(share->key).chmod;
    }
    if (destMapped)
        result.notes << QObject::tr("The destination %1 is reached over ssh: %2")
                            .arg(LocalPath::display(destination.path), LocalPath::display(result.destination.toString()));
    for (const Location &source : sources) {
        bool mapped = false;
        Location routed = map(source, &mapped);
        // rsync cannot copy between two hosts: such a source goes through the mount as before.
        if (mapped && result.destination.isRemote() && routed.originKey() != result.destination.originKey()) {
            routed = source;
            mapped = false;
        }
        // RsyncJob::plan() adds the "/" of "copy the contents" to local directories only; a location on the server
        // is passed as given, so it gets it here (the directory is looked at through the mount).
        if (mapped && contentsOnly && LocalPath::isDirectory(source.path) && !routed.path.endsWith(QLatin1Char('/')))
            routed.path += QLatin1Char('/');
        if (mapped)
            result.notes << QObject::tr("The source %1 is reached over ssh: %2")
                                .arg(LocalPath::display(source.path), LocalPath::display(routed.toString()));
        if (onNetworkMount(routed) && (result.destination.isRemote() || onNetworkMount(result.destination)))
            result.throughNetworkTwice = true;
        result.sources << routed;
    }
    return result;
}
