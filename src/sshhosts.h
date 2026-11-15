#pragma once

#include "location.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// The same server can be written in several ways: an alias from ~/.ssh/config ("nas-files"), its address with the
// user ("shareuser@10.0.0.6"), the server of a CIFS mount ("//10.0.0.6/Video"). ssh itself knows what they
// mean ("ssh -G" prints the configuration for a destination without connecting); this decides with it whether two
// locations are on the same server under the same account, so that rsync can run there.
struct SshIdentity
{
    QString user;
    QString hostname;
    int port = 0;

    bool isValid() const { return !hostname.isEmpty(); }
    bool operator==(const SshIdentity &other) const
    {
        return user == other.user && hostname.compare(other.hostname, Qt::CaseInsensitive) == 0 && port == other.port;
    }
};

namespace SshHosts {

// "ssh [the args of `sshCommand`] [-p port] -G -- target": the user, the host name and the port ssh would use.
// `sshCommand` is RsyncOptions::sshCommand() (program first; e.g. with "-F config"). Invalid if ssh fails or the
// program is not ssh.
SshIdentity resolve(const QStringList &sshCommand, const QString &target, int port);

// The identity of a remote location (cached by the caller if needed).
using Resolver = std::function<SshIdentity(const Location &location)>;

// Remote locations that are the same server and account under other names are rewritten to the spelling of the
// first one (the sources in order, then the destination): RsyncJob::plan() then sees one origin, and a copy within
// that server runs there. Returns a note for the log for every rewritten location.
QStringList unify(QList<Location> &sources, Location &destination, const Resolver &resolver);

// The Host entries of an ssh configuration file without wildcards (patterns, negations).
QStringList configAliases(const QString &configFile);
// The ssh configuration file used by `sshCommand`: its "-F file", else ~/.ssh/config.
QString configFile(const QStringList &sshCommand);
// The first alias of the configuration that leads to `server` (as an address or a name, compared with ssh's host
// name) and, if `user` is not empty, to that user; empty if none does.
QString aliasFor(const QStringList &sshCommand, const QString &server, const QString &user);

// The ssh login to suggest for the server of a network share: the spelling of a location of the copy at hand on
// that server (so that it matches the sources or the destination), else an alias from the ssh configuration, else
// "[user@]server".
QString suggestTarget(const QStringList &sshCommand, const QString &server, const QString &user,
                      const QList<Location> &candidates, const Resolver &resolver);

} // namespace SshHosts
