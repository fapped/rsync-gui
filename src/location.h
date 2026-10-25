#pragma once

#include <QString>

// A source or destination of a transfer: a local path or a remote path reached over ssh.
// Accepted forms: local paths (absolute, relative, with "~"), file:// URLs,
// sftp://[user@]host[:port]/path URLs (as passed by Dolphin) and rsync's [user@]host:path.
struct Location
{
    QString path;  // local: absolute clean path; remote: the path on the host
    QString user;  // remote only, may be empty
    QString host;  // empty for a local location
    int port = 0;  // remote only, 0 = the ssh default

    bool isValid() const { return !path.isEmpty(); }
    bool isRemote() const { return !host.isEmpty(); }

    // Parses user input; returns an invalid location for empty text.
    static Location parse(const QString &text);

    // The argument passed to rsync: "/local/path" or "user@host:/path".
    QString rsyncSpec() const;
    // A representation that parse() turns back into the same location (also shown in the GUI):
    // the local path, "user@host:path", or an sftp:// URL when a port is set.
    QString toString() const;
    // Remote locations with the same origin can be transferred by a single rsync process.
    QString originKey() const;

    bool operator==(const Location &other) const;
    bool operator!=(const Location &other) const { return !(*this == other); }
};
