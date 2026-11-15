#pragma once

#include "rsyncjob.h"
#include "rsyncoptions.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

// Checks done on a server over ssh before a transfer runs there (rsync on the server, see RsyncJob::Transfer::host):
// one ssh call runs a small POSIX sh script (busybox is enough) that reports whether the account runs commands at
// all, whether rsync is installed, the type and the real path of every source and of the destination, the nearest
// existing directory of the destination, whether it is writable and its free space.
class RemoteProbe : public QObject
{
    Q_OBJECT

public:
    enum class Type { Missing, File, Directory };

    struct PathInfo
    {
        QString path;     // as given (a path on the server)
        Type type = Type::Missing;
        QString realPath; // symlinks resolved; empty if it does not exist (or realpath is missing)
    };

    struct Result
    {
        bool ran = false;          // the script ran (the marker was printed)
        bool rsyncFound = false;
        QString rsyncVersion;      // the first line of "rsync --version"
        QList<PathInfo> sources;
        PathInfo destination;
        QString existingParent;    // the destination or its nearest existing parent directory
        QString existingParentReal; // its real path
        bool parentWritable = false;
        qint64 freeBytes = -1;     // in existingParent, -1 if unknown
        int exitCode = -1;
        bool cancelled = false;
        QString output;            // what the server printed besides the records (stderr, a banner...)

        // Empty when rsync can run on the server; else why not, for the user.
        QString error() const;
        // The sources that do not exist.
        QStringList missingSources() const;
        // The source directory that contains the destination (by the real paths), or empty.
        QString destinationInsideSource() const;
    };

    // The remote command (one argument for ssh): sh -c '<script>' rsync-gui-probe <destination> <sources...>
    static QString remoteCommand(const QStringList &sources, const QString &destination);
    // Parses the standard output of the script.
    static Result parse(const QByteArray &output, const QStringList &sources, const QString &destination);

    // The summed size of `paths` on the server in bytes (du -sk, so KiB precision): remote command and parser
    // (-1 if nothing could be measured).
    static QString sizeCommand(const QStringList &paths);
    static qint64 parseSize(const QByteArray &output);

    RemoteProbe(const RsyncOptions &options, const QString &host, int port, const QStringList &sources,
                const QString &destination, QObject *parent = nullptr);
    ~RemoteProbe() override;

    QString host() const { return m_host; }
    int port() const { return m_port; }
    void start();
    void cancel();
    // The ssh command that is run (for the log).
    RsyncJob::Command command() const;

signals:
    void finished(const RemoteProbe::Result &result);

private:
    RsyncOptions m_options;
    QString m_host;
    int m_port = 0;
    QStringList m_sources;
    QString m_destination;
    QPointer<QProcess> m_process;
    bool m_cancelled = false;
};

// A path on the server as an argument for the probe and the clean-up scripts: relative paths (to the home
// directory) get "./", so that no name can be taken for an option; "~" stays expandable (remotePathQuote()).
QString remoteScriptPath(const QString &path);
