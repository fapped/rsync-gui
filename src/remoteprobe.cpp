#include "remoteprobe.h"
#include "localpath.h"

#include <QDir>

namespace {

// POSIX sh, also busybox ash; records separated by NUL bytes (names may contain anything but NUL).
const char *const kScript = R"SH(printf 'RSYNC-GUI-PROBE\0'
if command -v rsync >/dev/null 2>&1; then
    printf 'rsync\0%s\0' "$(rsync --version 2>/dev/null | head -n 1)"
fi
info() {
    if [ -d "$2" ]; then t=dir; elif [ -e "$2" ] || [ -L "$2" ]; then t=file; else t=missing; fi
    r=
    if [ "$t" != missing ]; then r=$(realpath "$2" 2>/dev/null || readlink -f "$2" 2>/dev/null); fi
    printf '%s\0%s\0%s\0' "$1" "$t" "$r"
}
d=$1
shift
for p in "$@"; do info source "$p"; done
if [ -n "$d" ]; then
    info destination "$d"
    a=$d
    while [ ! -d "$a" ]; do
        n=$(dirname "$a")
        if [ "$n" = "$a" ]; then break; fi
        a=$n
    done
    w=no
    if [ -w "$a" ]; then w=yes; fi
    f=$(df -Pk "$a" 2>/dev/null | awk 'NR==2 {print $4}')
    r=$(realpath "$a" 2>/dev/null || readlink -f "$a" 2>/dev/null)
    printf 'parent\0%s\0%s\0%s\0%s\0' "$a" "$w" "$f" "$r"
fi
printf 'end\0'
)SH";

RemoteProbe::Type typeFromName(const QByteArray &name)
{
    if (name == "dir")
        return RemoteProbe::Type::Directory;
    if (name == "file")
        return RemoteProbe::Type::File;
    return RemoteProbe::Type::Missing;
}

bool isInside(const QString &path, const QString &dir)
{
    return path == dir || path.startsWith(dir == QStringLiteral("/") ? dir : dir + QLatin1Char('/'));
}

} // namespace

QString remoteScriptPath(const QString &path)
{
    if (path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('~')))
        return remotePathQuote(path);
    return remotePathQuote(QStringLiteral("./") + path);
}

QString RemoteProbe::Result::error() const
{
    const QString details = output.trimmed().isEmpty() ? QString() : QStringLiteral("\n\n") + output.trimmed();
    if (cancelled)
        return tr("The check of the server was cancelled.");
    if (!ran) {
        if (exitCode == 255)
            return tr("ssh could not connect to the server.") + details;
        return tr("The server did not run the check - rsync cannot run there. The ssh account may allow sftp "
                  "only, or its login shell is not a POSIX shell (sh, bash). rsync cannot copy to or from such an "
                  "account; mount it with sshfs (or copy it in Dolphin) and copy through the mount instead.")
            + details;
    }
    if (!rsyncFound)
        return tr("rsync is not installed on the server (or not in the PATH of the ssh user).");
    return {};
}

QStringList RemoteProbe::Result::missingSources() const
{
    QStringList missing;
    for (const PathInfo &source : sources) {
        if (source.type == Type::Missing)
            missing << source.path;
    }
    return missing;
}

QString RemoteProbe::Result::destinationInsideSource() const
{
    // The real path of the destination; if it does not exist yet, that of its nearest existing parent plus the rest.
    QString destReal = destination.realPath;
    if (destReal.isEmpty() && !existingParentReal.isEmpty()) {
        const QString dest = QDir::cleanPath(destination.path);
        const QString parent = QDir::cleanPath(existingParent);
        const QString rest = dest.startsWith(parent) ? dest.mid(parent.size()) : QString();
        destReal = QDir::cleanPath(existingParentReal + QLatin1Char('/') + rest);
    }
    if (destReal.isEmpty())
        return {};
    for (const PathInfo &source : sources) {
        if (source.type == Type::Directory && !source.realPath.isEmpty() && isInside(destReal, source.realPath))
            return source.path;
    }
    return {};
}

QString RemoteProbe::remoteCommand(const QStringList &sources, const QString &destination)
{
    QStringList words{QStringLiteral("sh"), QStringLiteral("-c"), remoteShellQuote(QString::fromUtf8(kScript)),
                      QStringLiteral("rsync-gui-probe"),
                      destination.isEmpty() ? QStringLiteral("''") : remoteScriptPath(destination)};
    for (const QString &source : sources)
        words << remoteScriptPath(source);
    return words.join(QLatin1Char(' '));
}

RemoteProbe::Result RemoteProbe::parse(const QByteArray &output, const QStringList &sources, const QString &destination)
{
    Result result;
    result.destination.path = destination;
    QList<QByteArray> fields = output.split('\0');
    // Anything before the marker (e.g. a message of the login shell) is kept for the user.
    const qsizetype marker = fields.indexOf("RSYNC-GUI-PROBE");
    if (marker < 0) {
        result.output = QString::fromLocal8Bit(output);
        return result;
    }
    if (marker > 0)
        result.output = QString::fromLocal8Bit(fields.at(marker - 1));
    result.ran = true;
    qsizetype i = marker + 1;
    const auto next = [&fields, &i]() -> QByteArray { return i < fields.size() ? fields.at(i++) : QByteArray(); };
    qsizetype sourceIndex = 0;
    while (i < fields.size()) {
        const QByteArray tag = next();
        if (tag == "rsync") {
            result.rsyncFound = true;
            result.rsyncVersion = QString::fromLocal8Bit(next()).trimmed();
        } else if (tag == "source" || tag == "destination") {
            PathInfo info;
            info.type = typeFromName(next());
            info.realPath = LocalPath::decode(next());
            if (tag == "destination") {
                info.path = destination;
                result.destination = info;
                result.destinationChecked = true;
            } else {
                info.path = sourceIndex < sources.size() ? sources.at(sourceIndex) : QString();
                ++sourceIndex;
                result.sources << info;
            }
        } else if (tag == "parent") {
            result.existingParent = LocalPath::decode(next());
            result.parentWritable = next() == "yes";
            bool ok = false;
            const qint64 kib = next().trimmed().toLongLong(&ok);
            result.freeBytes = ok ? kib * 1024 : -1;
            result.existingParentReal = LocalPath::decode(next());
        } else if (tag == "end" || tag.isEmpty()) {
            break;
        }
    }
    return result;
}

QString RemoteProbe::sizeCommand(const QStringList &paths)
{
    QStringList words{QStringLiteral("du"), QStringLiteral("-sk")};
    for (const QString &path : paths)
        words << remoteScriptPath(path);
    return words.join(QLatin1Char(' ')) + QStringLiteral(" 2>/dev/null");
}

qint64 RemoteProbe::parseSize(const QByteArray &output)
{
    qint64 total = 0;
    bool any = false;
    for (const QByteArray &line : output.split('\n')) {
        const qsizetype tab = line.indexOf('\t');
        bool ok = false;
        const qint64 kib = line.left(tab).trimmed().toLongLong(&ok);
        if (tab > 0 && ok) {
            total += kib * 1024;
            any = true;
        }
    }
    return any ? total : -1;
}

std::optional<RemoteProbe::Target> RemoteProbe::targetFor(const RsyncJob::Transfer &transfer)
{
    Target target;
    target.port = transfer.port;
    if (transfer.runsOnServer()) {
        target.kind = Target::Kind::OnServer;
        target.host = transfer.host;
        target.sources = transfer.sources;
        target.sourceSpecs = transfer.sources;
        target.destination = transfer.destination;
        return target;
    }
    const auto hostOf = [](const Location &loc) { return loc.user.isEmpty() ? loc.host : loc.user + QLatin1Char('@') + loc.host; };
    // rsync copies from one remote host, or to one: all sources of a transfer have the same origin.
    const Location first = transfer.sources.isEmpty() ? Location() : Location::parse(transfer.sources.first());
    if (first.isRemote()) {
        target.kind = Target::Kind::From;
        target.host = hostOf(first);
        for (const QString &spec : transfer.sources) {
            target.sources << Location::parse(spec).path;
            target.sourceSpecs << spec;
        }
        return target;
    }
    const Location dest = Location::parse(transfer.destination);
    if (dest.isRemote()) {
        target.kind = Target::Kind::To;
        target.host = hostOf(dest);
        target.destination = dest.path;
        return target;
    }
    return std::nullopt;
}

RemoteProbe::RemoteProbe(const RsyncOptions &options, const QString &host, int port, const QStringList &sources,
                         const QString &destination, QObject *parent)
    : QObject(parent), m_options(options), m_host(host), m_port(port), m_sources(sources), m_destination(destination)
{
}

RemoteProbe::~RemoteProbe()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

RsyncJob::Command RemoteProbe::command() const
{
    QStringList ssh = m_options.sshCommand(m_port);
    const QString program = ssh.takeFirst();
    return {program, ssh << QStringLiteral("--") << m_host << remoteCommand(m_sources, m_destination)};
}

void RemoteProbe::start()
{
    const RsyncJob::Command cmd = command();
    m_process = new QProcess(this);
    m_process->setStandardInputFile(QProcess::nullDevice());
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        Result result = parse(m_process->readAllStandardOutput(), m_sources, m_destination);
        const QString errors = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
        if (!errors.isEmpty())
            result.output = result.output.isEmpty() ? errors : result.output + QLatin1Char('\n') + errors;
        result.exitCode = status == QProcess::NormalExit ? exitCode : -1;
        result.cancelled = m_cancelled;
        m_process->deleteLater();
        m_process = nullptr;
        emit finished(result);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || !m_process)
            return;
        Result result;
        result.output = m_process->errorString();
        result.cancelled = m_cancelled;
        m_process->deleteLater();
        m_process = nullptr;
        emit finished(result);
    });
    RsyncJob::setRawArguments(m_process, cmd.program, cmd.arguments);
    // No timeout of its own: ssh may wait for a password typed by the user; Cancel stops it.
    m_process->start(cmd.program, cmd.arguments);
}

void RemoteProbe::cancel()
{
    m_cancelled = true;
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->terminate();
}
