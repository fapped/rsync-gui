#pragma once

#include "location.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

class QSettings;

// A mounted file system from /proc/self/mountinfo.
struct Mount
{
    QString mountPoint;
    QString root;     // the directory of the file system mounted there ("/" for the whole one)
    QString fsType;   // e.g. "cifs", "fuse.sshfs"
    QString source;   // e.g. "//server/share", "user@host:/path"
    QString superOptions;

    // The entries of /proc/self/mountinfo (escapes such as "\040" decoded).
    static QList<Mount> parse(const QByteArray &mountinfo);
    static QList<Mount> current();
    // The mount that contains `path` (the longest mount point that is a prefix of it).
    static std::optional<Mount> find(const QList<Mount> &mounts, const QString &path);
};

// A network file system that is also reachable over ssh: sshfs (the server and the path are in the mount) and
// CIFS/SMB (the server is; the path of the share on the server is not - see sharePathCommand()).
struct NetworkShare
{
    enum class Kind { Sshfs, Cifs };

    Kind kind = Kind::Cifs;
    QString key;        // identifies the share in the settings: "//server/share" or "sshfs:user@host:path"
    QString mountPoint;
    QString server;     // the host name or address from the mount
    QString user;       // sshfs: the user from the mount; CIFS: the username= option (a guess for ssh)
    QString share;      // CIFS: the name of the share
    QString remotePath; // sshfs: the path on the server ("" = the home directory); CIFS: the prefix path inside the share
    QString display() const; // "//server/share" or "user@host:/path"

    // A network share for a mount, if it is one.
    static std::optional<NetworkShare> fromMount(const Mount &mount);
    // A share known only by its key (from the settings, not mounted now; no mount point).
    static NetworkShare fromKey(const QString &key);
    // The share of a local path and the part of the path inside it ("" for the mount point itself).
    static std::optional<NetworkShare> of(const QList<Mount> &mounts, const QString &localPath, QString *relative = nullptr);

    // The remote command that prints the path of a Samba share on the server (testparm).
    static QString sharePathCommand(const QString &share);
    // The remote command that prints the Samba parameters that decide the owner and the mode of new files,
    // one "name=value" line each (force user, force group, create mask, force create mode, directory mask,
    // force directory mode).
    static QString ownershipCommand(const QString &share);
    // The "name=value" lines of ownershipCommand() (empty values left out).
    static QMap<QString, QString> parseParameters(const QByteArray &output);
    // rsync's --chmod value that gives files and directories copied over ssh the modes Samba would give them:
    // Samba sets (mode & create mask) | force create mode (directory mask / force directory mode for
    // directories), --chmod removes and adds the same bits symbolically, e.g. "Fg-wx,Fo-wx". Empty if nothing
    // changes. Missing parameters take Samba's defaults (0744, 0755, 0000).
    static QString chmodFromSamba(const QMap<QString, QString> &parameters);
};

// How a network share is reached over ssh (remembered per share, see load()/save()).
struct ShareSettings
{
    enum class Use {
        Unset,   // not decided yet: the window offers to set it up when the share is used
        Enabled, // copied over ssh / on the server instead of through the mount
        Disabled,
        Never,   // through the mount and never asked again
    };

    Use use = Use::Unset;
    QString sshTarget;  // [user@]host
    int port = 0;
    QString serverPath; // the path of the mount point on the server ("~/x" or "x" relative to the home directory)
    QString chmod;      // rsync --chmod for copies to the share over ssh (e.g. Samba's masks), empty = none

    bool isUsable() const { return use == Use::Enabled && !sshTarget.isEmpty() && !serverPath.isEmpty(); }
    // A location on the server for `relative` (a path inside the share).
    Location location(const QString &relative) const;

    // The values detected from the mount: sshfs - all of them; CIFS - the server and the user only.
    static ShareSettings detected(const NetworkShare &share);
    static ShareSettings load(QSettings &settings, const QString &key);
    void save(QSettings &settings, const QString &key) const;
    // The keys of all shares with saved settings.
    static QStringList savedKeys(QSettings &settings);
    static void remove(QSettings &settings, const QString &key);
};

// Decides how the sources and the destination of a copy are reached when some of them are on network shares:
// a share that is set up for ssh is replaced by its location on the server. Then rsync runs on the server
// (both on one host), over ssh (one side here) or, where that is not possible (sources on another host than a
// remote destination), through the mount as before.
struct ShareRouting
{
    QList<Location> sources;
    Location destination;
    QStringList notes;      // for the log: what goes where
    QString destinationChmod; // --chmod of the share of a destination reached over ssh (ShareSettings::chmod)
    QStringList unsetKeys;  // shares used here that are not set up yet (Use::Unset), to offer setting them up
    QList<NetworkShare> unsetShares;
    bool throughNetworkTwice = false; // a source on a network mount copied to a network destination through here

    using SettingsLookup = std::function<ShareSettings(const QString &key)>;
    static ShareRouting route(const QList<Location> &sources, const Location &destination, const QList<Mount> &mounts,
                              const SettingsLookup &settings, bool useServer, bool contentsOnly = false);
};
