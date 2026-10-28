#include "rsyncjob.h"
#include "localpath.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

namespace {

bool localExists(const QString &path)
{
    return LocalPath::info(path).exists; // also a dangling symlink, it is copied as a symlink
}

QString canonicalOrClean(const QString &path)
{
    return LocalPath::canonical(path);
}

bool isInside(const QString &path, const QString &dir)
{
    return path == dir || path.startsWith(dir == QStringLiteral("/") ? dir : dir + QLatin1Char('/'));
}

// Lines that are not names of transferred files.
bool isStatusLine(const QString &line)
{
    static const QStringList prefixes{
        QStringLiteral("sending incremental file list"),
        QStringLiteral("receiving incremental file list"),
        QStringLiteral("sending file list"),
        QStringLiteral("receiving file list"),
        QStringLiteral("building file list"),
        QStringLiteral("created directory "),
        QStringLiteral("deleting "),
        QStringLiteral("sent "),
        QStringLiteral("total size is "),
        QStringLiteral("rsync: "),
        QStringLiteral("rsync error: "),
        QStringLiteral("rsync warning: "),
        QStringLiteral("file has vanished: "),
        QStringLiteral("skipping "),
        QStringLiteral("("),
    };
    for (const QString &prefix : prefixes) {
        if (line.startsWith(prefix))
            return true;
    }
    return false;
}

} // namespace

RsyncJob::Plan RsyncJob::plan(const QList<Location> &sources, const Location &destination,
                              const RsyncOptions &options)
{
    Plan result;
    if (!destination.isValid()) {
        result.error = tr("No destination selected.");
        return result;
    }
    if (options.verifyAfter && !options.canVerify())
        result.notes << tr("Verification is skipped for a dry run.");

    QString destSpec = destination.rsyncSpec();
    if (!destSpec.endsWith(QLatin1Char('/')))
        destSpec += QLatin1Char('/'); // always copy *into* the destination directory
    const QString destCanonical = destination.isRemote() ? QString() : canonicalOrClean(destination.path);

    QHash<QString, qsizetype> transferByOrigin;
    QStringList seen;
    QStringList errors;
    for (const Location &source : sources) {
        if (!source.isValid())
            continue;
        QString spec = source.rsyncSpec();
        QString sourceDir;
        if (source.isRemote()) {
            if (destination.isRemote()) {
                errors << tr("%1: rsync cannot copy between two remote locations.").arg(source.toString());
                continue;
            }
        } else {
            if (!localExists(source.path)) {
                result.missing << source.path;
                continue;
            }
            const QString canonical = canonicalOrClean(source.path);
            const bool isDir = LocalPath::info(source.path).isDir;
            if (isDir && !destination.isRemote() && isInside(destCanonical, canonical)) {
                errors << tr("The destination %1 is inside the source directory %2.")
                              .arg(LocalPath::display(destination.path), LocalPath::display(source.path));
                continue;
            }
            if (isDir && options.contentsOnly && !spec.endsWith(QLatin1Char('/')))
                spec += QLatin1Char('/');
            if (isDir)
                sourceDir = source.path;
        }

        const QString key = source.originKey() + QLatin1Char('\n') + spec;
        if (seen.contains(key)) {
            result.notes << tr("Duplicate source skipped: %1").arg(LocalPath::display(source.toString()));
            continue;
        }
        seen << key;

        const QString origin = source.originKey();
        auto it = transferByOrigin.constFind(origin);
        if (it == transferByOrigin.cend()) {
            Transfer transfer;
            transfer.destination = destSpec;
            transfer.port = source.isRemote() ? source.port : destination.port;
            if (!source.isRemote())
                transfer.description = tr("local files");
            else if (source.port > 0)
                transfer.description = tr("from %1 (port %2)").arg(source.host).arg(source.port);
            else
                transfer.description = tr("from %1").arg(source.host);
            it = transferByOrigin.insert(origin, result.transfers.size());
            result.transfers << transfer;
        }
        result.transfers[*it].sources << spec;
        if (!sourceDir.isEmpty())
            result.transfers[*it].sourceDirs << sourceDir;
    }

    if (!errors.isEmpty())
        result.error = errors.join(QLatin1Char('\n'));
    else if (result.transfers.isEmpty())
        result.error = result.missing.isEmpty() ? tr("No sources selected.")
                                                : tr("None of the sources exists:\n%1")
                                                      .arg(LocalPath::display(result.missing.join(QLatin1Char('\n'))));
    return result;
}

bool RsyncJob::parseProgressLine(const QString &line, ProgressInfo *info, int humanLevel)
{
    // "      3,00M  99%    2,76GB/s    0:00:00 (xfr#1, to-chk=3/5)"
    static const QRegularExpression re(QStringLiteral(
        "^\\s*([0-9][0-9.,]*[A-Za-z]?)\\s+([0-9]{1,3})%\\s+(\\S+)\\s+([0-9]+:[0-9]{2}:[0-9]{2})"
        "(?:\\s+\\((?:xfr#([0-9]+),\\s*)?(to|ir)-chk=([0-9]+)/([0-9]+)\\))?\\s*$"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
        return false;
    if (info) {
        *info = ProgressInfo();
        info->transferred = m.captured(1);
        info->percent = qBound(0, m.captured(2).toInt(), 100);
        info->rate = m.captured(3);
        info->eta = m.captured(4);
        if (m.hasCaptured(5))
            info->transferNumber = m.captured(5).toInt();
        if (m.hasCaptured(6)) {
            info->totalFinal = m.captured(6) == QStringLiteral("to");
            info->remaining = m.captured(7).toInt();
            info->total = m.captured(8).toInt();
        }
        info->bytes = parseSize(info->transferred, humanLevel);
        info->bytesPerSecond = parseRate(info->rate);
    }
    return true;
}

namespace {

// "4,62" or "4.62" (the decimal separator follows the locale of rsync).
double parseDecimal(QString number)
{
    number.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double value = number.toDouble(&ok);
    return ok ? value : -1;
}

int unitPower(QChar unit)
{
    static const QString units = QStringLiteral("KMGTPE");
    return units.indexOf(unit.toUpper()) + 1; // 0 if it is not a unit
}

} // namespace

qint64 RsyncJob::parseSize(const QString &text, int humanLevel)
{
    if (text.isEmpty())
        return -1;
    const int power = unitPower(text.back());
    if (power == 0) {
        // Plain number with thousands separators (which also follow the locale): only the digits count.
        QString digits;
        for (const QChar c : text) {
            if (c.isDigit())
                digits += c;
        }
        bool ok = false;
        const qint64 value = digits.toLongLong(&ok);
        return ok ? value : -1;
    }
    const double value = parseDecimal(text.chopped(1));
    if (value < 0)
        return -1;
    const double base = humanLevel >= 3 ? 1024.0 : 1000.0;
    return qint64(value * std::pow(base, power));
}

qint64 RsyncJob::parseRate(const QString &text)
{
    // "2,00MB/s", "0,00kB/s": always units of 1024, whatever the -h level.
    static const QRegularExpression re(QStringLiteral("^([0-9]+(?:[.,][0-9]+)?)([kKMGTPE])B/s$"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return -1;
    const double value = parseDecimal(m.captured(1));
    return value < 0 ? -1 : qint64(value * std::pow(1024.0, unitPower(m.captured(2).at(0))));
}

int RsyncJob::humanLevel(const QStringList &args)
{
    int level = 1;
    for (const QString &arg : args) {
        if (arg == QStringLiteral("--"))
            break;
        if (arg == QStringLiteral("--human-readable"))
            ++level;
        else if (arg.startsWith(QLatin1Char('-')) && !arg.startsWith(QStringLiteral("--")))
            level += int(arg.count(QLatin1Char('h')));
    }
    return level;
}

bool RsyncJob::parseDifferenceLine(const QString &line, QString *name)
{
    // %i is "YXcstpoguax": Y is the update type ('<' / '>' a file transfer, 'c' a created entry,
    // 'h' a hard link, '.' no transfer), X the file type.
    static const QRegularExpression re(QStringLiteral("^[<>ch][fdLDS][^ ]{9,} (.+)$"));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
        return false;
    if (name)
        *name = m.captured(1);
    return true;
}

bool RsyncJob::isFatFileSystem(const QString &path)
{
    constexpr long kMsdosSuperMagic = 0x4d44; // vfat and msdos, see statfs(2)
    struct statfs fs;
    return ::statfs(LocalPath::encode(path).constData(), &fs) == 0 && long(fs.f_type) == kMsdosSuperMagic;
}

int RsyncJob::modifyWindowFor(const QString &localDestination, bool mayWrite)
{
    // The destination may not exist yet (rsync or the window creates it) - its parent decides then.
    QByteArray dir = LocalPath::encode(QDir::cleanPath(localDestination));
    struct stat st;
    while (::stat(dir.constData(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        const qsizetype slash = dir.lastIndexOf('/');
        if (slash <= 0)
            return 0;
        dir.truncate(slash);
    }
    const int byType = isFatFileSystem(LocalPath::decode(dir)) ? 1 : 0;
    if (!mayWrite)
        return byType;

    QByteArray probe = dir + "/.rsync-gui-time-XXXXXX";
    const int fd = ::mkostemp(probe.data(), O_CLOEXEC);
    if (fd < 0)
        return byType;
    // 2001-09-09 01:46:41 UTC - an odd second, as precise as rsync compares (it ignores nanoseconds).
    constexpr time_t kOddSecond = 1000000001;
    const struct timespec times[2] = {{kOddSecond, 0}, {kOddSecond, 0}};
    int window = byType;
    if (::futimens(fd, times) == 0) {
        // Read back by name (not fstat): some file systems (e.g. FUSE) round only when they store the time.
        ::close(fd);
        struct stat result;
        if (::stat(probe.constData(), &result) == 0)
            window = result.st_mtim.tv_sec == kOddSecond ? 0 : 1;
    } else {
        ::close(fd);
    }
    ::unlink(probe.constData());
    return window;
}

QString RsyncJob::exitCodeDescription(int code)
{
    switch (code) {
    case 0: return tr("success");
    case 1: return tr("syntax or usage error");
    case 2: return tr("protocol incompatibility");
    case 3: return tr("errors selecting input/output files or directories");
    case 4: return tr("requested action not supported");
    case 5: return tr("error starting the client-server protocol");
    case 10: return tr("error in socket I/O");
    case 11: return tr("error in file I/O");
    case 12: return tr("error in the rsync protocol data stream");
    case 14: return tr("error in IPC code");
    case 20: return tr("interrupted (SIGUSR1 or SIGINT)");
    case 22: return tr("error allocating memory");
    case 23: return tr("partial transfer due to an error");
    case 24: return tr("partial transfer: some source files vanished");
    case 25: return tr("the --max-delete limit stopped deletions");
    case 30: return tr("timeout in data send/receive");
    case 35: return tr("timeout waiting for the daemon connection");
    case 255: return tr("remote shell (ssh) error");
    default: return tr("error");
    }
}

RsyncJob::RsyncJob(QList<Transfer> transfers, RsyncOptions options, QObject *parent)
    : QObject(parent)
    , m_transfers(std::move(transfers))
    , m_options(std::move(options))
{
    m_stepsPerTransfer = m_options.verifiedMove() ? 3 : m_options.canVerify() ? 2 : 1;
}

RsyncJob::~RsyncJob()
{
    if (m_dropThread) {
        m_dropCancel->store(true);
        m_dropThread->disconnect(this);
        m_dropThread->wait();
    }
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void RsyncJob::start()
{
    if (m_running)
        return;
    m_running = true;
    m_index = 0;
    m_phase = Phase::Copy;
    m_bytesBefore = 0;
    m_summary = {};
    m_differingFiles.clear();
    m_cancelRequested = false;
    emit progress(0);
    startNext();
}

void RsyncJob::cancel()
{
    if (!m_running)
        return;
    m_cancelRequested = true;
    emit log(LogLevel::Warning, tr("Cancelling..."));
    if (m_dropCancel)
        m_dropCancel->store(true);
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        QPointer<QProcess> process = m_process;
        QTimer::singleShot(3000, this, [process] {
            if (process && process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
}

void RsyncJob::startNext()
{
    if (m_cancelRequested || m_index >= m_transfers.size()) {
        m_summary.cancelled = m_cancelRequested;
        finish();
        return;
    }
    startProcess();
}

void RsyncJob::startProcess()
{
    const Transfer &transfer = m_transfers.at(m_index);
    QStringList args;
    QString phase;
    switch (m_phase) {
    case Phase::Copy:
        args = m_options.arguments(transfer.port);
        phase = tr("Copying");
        break;
    case Phase::Verify:
        args = m_options.verifyArguments(transfer.port);
        phase = tr("Verifying");
        break;
    case Phase::Remove:
        args = m_options.removalArguments(transfer.port);
        phase = tr("Removing the sources");
        break;
    }
    args << QStringLiteral("--") << transfer.sources << transfer.destination;

    if (m_transfers.size() > 1)
        phase = tr("%1 %2 (%3/%4)").arg(phase, transfer.description).arg(m_index + 1).arg(m_transfers.size());
    emit phaseChanged(phase);
    emit log(LogLevel::Info, QStringLiteral("--- %1 ---").arg(phase));
    emit log(LogLevel::Info, QStringLiteral("$ ") + commandLine(m_options.program, args));
    emit statusChanged(QString());
    emitProgress(0.0);

    m_pending.clear();
    m_lastDone = 0;
    m_lastTotal = 0;
    m_humanLevel = humanLevel(args);
    m_completedBytes = 0;
    m_processedBytes = 0;
    m_lastTransferNumber = 0; // (the bytes of a finished copy are already in m_bytesBefore)
    m_transferDifferences = 0;
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_process, &QProcess::readyRead, this, &RsyncJob::onReadyRead);
    connect(m_process, &QProcess::finished, this, &RsyncJob::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &RsyncJob::onProcessError);
    setRawArguments(m_process, args);
    m_process->start(m_options.program, args);
}

void RsyncJob::setRawArguments(QProcess *process, const QStringList &args) const
{
    // QProcess passes arguments as UTF-8, which would change file names that are not valid UTF-8
    // (see LocalPath). For such arguments the child process execs rsync itself with the exact bytes;
    // if that fails, QProcess continues with its own exec, which then fails the same way.
    if (std::all_of(args.cbegin(), args.cend(), [](const QString &arg) { return LocalPath::isPlain(arg); }))
        return;
    const QString program = m_options.program.contains(QLatin1Char('/'))
        ? m_options.program
        : QStandardPaths::findExecutable(m_options.program);
    if (program.isEmpty())
        return;
    auto bytes = std::make_shared<std::vector<QByteArray>>();
    bytes->push_back(LocalPath::encode(program));
    for (const QString &arg : args)
        bytes->push_back(LocalPath::encode(arg));
    auto argv = std::make_shared<std::vector<char *>>();
    for (QByteArray &arg : *bytes)
        argv->push_back(arg.data()); // detached here, in the parent
    argv->push_back(nullptr);
    // Runs in the child after fork(); only async-signal-safe calls.
    process->setChildProcessModifier([bytes, argv] { ::execv(argv->front(), argv->data()); });
}

void RsyncJob::onReadyRead()
{
    if (!m_process)
        return;
    m_pending += m_process->readAll();
    // Progress updates are separated by '\r', regular lines by '\n'.
    qsizetype start = 0;
    for (qsizetype i = 0; i < m_pending.size(); ++i) {
        const char c = m_pending.at(i);
        if (c == '\n' || c == '\r') {
            handleLine(QString::fromLocal8Bit(m_pending.mid(start, i - start)));
            start = i + 1;
        }
    }
    m_pending.remove(0, start);
}

void RsyncJob::handleLine(const QString &rawLine)
{
    const QString line = rawLine.trimmed();
    if (line.isEmpty())
        return;

    ProgressInfo info;
    if (parseProgressLine(line, &info, m_humanLevel)) {
        if (m_phase == Phase::Copy && info.bytes >= 0) {
            if (m_options.progress == RsyncOptions::Progress::Overall) {
                m_processedBytes = info.bytes; // the whole transfer so far
            } else if (info.transferNumber > m_lastTransferNumber) {
                // The last line of a file (repeated for following entries with the same number).
                m_lastTransferNumber = info.transferNumber;
                m_completedBytes += info.bytes;
                m_processedBytes = m_completedBytes;
            } else if (info.transferNumber < 0) {
                m_processedBytes = m_completedBytes + info.bytes; // the current file
            }
            if (info.bytesPerSecond >= 0)
                emit transferRateChanged(info.bytesPerSecond, m_bytesBefore + m_processedBytes);
        }
        double fraction = 0.0;
        if (info.total > 0) {
            m_lastTotal = info.total;
            m_lastDone = info.total - info.remaining;
        }
        if (m_options.progress == RsyncOptions::Progress::Overall) {
            fraction = info.percent / 100.0;
        } else if (m_lastTotal > 0) {
            // Files finished so far plus the part of the current file.
            const bool fileDone = info.total > 0;
            fraction = (m_lastDone + (fileDone ? 0.0 : info.percent / 100.0)) / qMax(m_lastTotal, m_lastDone + 1);
        } else {
            fraction = info.percent / 100.0;
        }
        emitProgress(qBound(0.0, fraction, 1.0));

        QString status = tr("%1%  %2  %3  ETA %4").arg(info.percent).arg(info.transferred, info.rate, info.eta);
        if (info.total > 0) {
            status += info.totalFinal ? tr("  files: %1/%2").arg(info.total - info.remaining).arg(info.total)
                                      : tr("  files: %1/%2+").arg(info.total - info.remaining).arg(info.total);
        }
        emit statusChanged(status);
        return;
    }

    if (m_phase == Phase::Verify) {
        QString name;
        if (parseDifferenceLine(line, &name)) {
            ++m_transferDifferences;
            if (m_differingFiles.size() < 100000)
                m_differingFiles << name;
            emit log(LogLevel::Warning, tr("Differs: %1").arg(line));
            return;
        }
    }

    LogLevel level = LogLevel::Output;
    if (line.startsWith(QStringLiteral("rsync: ")) || line.startsWith(QStringLiteral("rsync error: ")))
        level = LogLevel::Error;
    else if (line.startsWith(QStringLiteral("rsync warning: ")) || line.startsWith(QStringLiteral("file has vanished: ")))
        level = LogLevel::Warning;
    emit log(level, line);

    if (m_phase == Phase::Copy && !isStatusLine(line)) {
        const qsizetype arrow = line.indexOf(QStringLiteral(" -> "));
        emit currentFileChanged(arrow > 0 ? line.left(arrow) : line);
    }
}

void RsyncJob::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    onReadyRead();
    if (!m_pending.isEmpty())
        handleLine(QString::fromLocal8Bit(m_pending));
    m_pending.clear();
    m_process->deleteLater();
    m_process = nullptr;
    emit currentFileChanged(QString());
    emit statusChanged(QString());

    const bool success = status == QProcess::NormalExit && exitCode == 0;
    const QString codeText = status == QProcess::CrashExit
        ? tr("rsync crashed")
        : tr("exit code %1: %2").arg(exitCode).arg(exitCodeDescription(exitCode));

    if (m_phase == Phase::Copy) {
        m_bytesBefore += m_processedBytes;
        m_processedBytes = 0;
        bool runVerify = false;
        if (m_cancelRequested) {
            ++m_summary.transfersFailed;
            emit log(LogLevel::Warning, tr("Transfer cancelled (%1). Partially copied files may remain.").arg(codeText));
        } else if (success) {
            ++m_summary.transfersOk;
            emit log(LogLevel::Success, tr("Transfer finished."));
            if (!m_options.verifiedMove())
                removeEmptiedSourceDirs(m_transfers.at(m_index));
            runVerify = m_options.canVerify();
        } else if (status == QProcess::NormalExit && exitCode == 24) {
            ++m_summary.transfersWithWarnings;
            emit log(LogLevel::Warning, tr("Transfer finished with warnings (%1).").arg(codeText));
            if (!m_options.verifiedMove())
                removeEmptiedSourceDirs(m_transfers.at(m_index));
            runVerify = m_options.canVerify();
        } else {
            ++m_summary.transfersFailed;
            emit log(LogLevel::Error, tr("Transfer failed (%1).").arg(codeText));
            if (m_options.canVerify())
                emit log(LogLevel::Warning, tr("Verification skipped because the transfer failed."));
            if (m_options.verifiedMove())
                emit log(LogLevel::Warning, tr("The sources were not removed."));
        }
        if (runVerify) {
            m_phase = Phase::Verify;
            startVerify();
            return;
        }
    } else if (m_phase == Phase::Verify) {
        if (m_cancelRequested) {
            ++m_summary.verifyFailed;
            emit log(LogLevel::Warning, tr("Verification cancelled."));
        } else if (!success) {
            ++m_summary.verifyFailed;
            emit log(LogLevel::Error, tr("Verification failed (%1).").arg(codeText));
        } else if (m_transferDifferences > 0) {
            m_summary.differences += m_transferDifferences;
            const QString text = tr("Verification: %1 entr(y/ies) in the destination differ from the source or are missing.")
                                     .arg(m_transferDifferences);
            if (m_options.existing != RsyncOptions::Existing::Replace)
                emit log(LogLevel::Error, text + tr(" Existing files were not replaced (-u / --ignore-existing), "
                                                    "which may explain the differences."));
            else
                emit log(LogLevel::Error, text);
        } else {
            ++m_summary.verified;
            emit log(LogLevel::Success, tr("Verification passed: the content of the destination matches the source."));
            if (m_options.verifiedMove()) {
                m_phase = Phase::Remove;
                startProcess();
                return;
            }
        }
        if (m_options.verifiedMove())
            emit log(LogLevel::Warning, tr("The sources were not removed, because the verification did not pass."));
    } else {
        // The removal pass of a verified move.
        if (m_cancelRequested) {
            ++m_summary.removalFailed;
            emit log(LogLevel::Warning, tr("Removal cancelled - some sources may have been removed already."));
        } else if (success || (status == QProcess::NormalExit && exitCode == 24)) {
            ++m_summary.sourcesRemoved;
            emit log(LogLevel::Success, tr("The sources were removed."));
            removeEmptiedSourceDirs(m_transfers.at(m_index));
        } else {
            ++m_summary.removalFailed;
            emit log(LogLevel::Error, tr("Removing the sources failed (%1).").arg(codeText));
        }
    }

    m_phase = Phase::Copy;
    ++m_index;
    startNext();
}

void RsyncJob::startVerify()
{
    const Transfer &transfer = m_transfers.at(m_index);
    if (Location::parse(transfer.destination).isRemote()) {
        emit log(LogLevel::Info, tr("The destination is remote: the verification may read the copies from the "
                                    "server's memory (its page cache) instead of its disk."));
        startProcess();
        return;
    }
    // Right after copying, the copies are still in memory: without this the verification would compare the
    // source with the page cache, not with what is on the disk.
    const QString phase = tr("Writing to the disk");
    emit phaseChanged(phase);
    emit log(LogLevel::Info, QStringLiteral("--- %1 ---").arg(phase));
    emit statusChanged(QString());
    m_dropCancel = std::make_shared<std::atomic<bool>>(m_cancelRequested);
    auto result = std::make_shared<CacheDropResult>();
    QThread *thread = QThread::create([paths = destinationPaths(transfer), cancel = m_dropCancel, result] {
        *result = dropFromPageCache(paths, cancel.get());
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, result] { onCacheDropped(*result); });
    m_dropThread = thread;
    thread->start();
}

void RsyncJob::onCacheDropped(const CacheDropResult &result)
{
    m_dropThread = nullptr;
    m_dropCancel.reset();
    if (m_cancelRequested || result.cancelled) {
        ++m_summary.verifyFailed;
        emit log(LogLevel::Warning, tr("Verification cancelled."));
        if (m_options.verifiedMove())
            emit log(LogLevel::Warning, tr("The sources were not removed, because the verification did not pass."));
        m_phase = Phase::Copy;
        ++m_index;
        startNext();
        return;
    }
    emit log(LogLevel::Info, tr("%n file(s) (%1) written to the disk and dropped from the cache - the verification "
                                "reads them from the disk.", nullptr, int(result.files))
                                 .arg(QLocale().formattedDataSize(result.bytes)));
    if (result.errors > 0)
        emit log(LogLevel::Warning, tr("%n file(s) could not be opened - they may be read from the cache.", nullptr,
                                       result.errors));
    startProcess();
}

QStringList RsyncJob::destinationPaths(const Transfer &transfer)
{
    QString dest = transfer.destination;
    while (dest.size() > 1 && dest.endsWith(QLatin1Char('/')))
        dest.chop(1);
    QStringList paths;
    for (const QString &source : transfer.sources) {
        const Location location = Location::parse(source);
        if (source.endsWith(QLatin1Char('/'))) {
            if (location.isRemote())
                return {dest};
            // The contents of a local directory: its entries are in the destination.
            const QByteArray dir = LocalPath::encode(location.path);
            DIR *d = ::opendir(dir.constData());
            if (!d)
                return {dest};
            while (dirent *entry = ::readdir(d)) {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                    paths << dest + QLatin1Char('/') + LocalPath::decode(QByteArray(entry->d_name));
            }
            ::closedir(d);
        } else {
            QString name = location.path;
            name = name.mid(name.lastIndexOf(QLatin1Char('/')) + 1);
            if (name.isEmpty())
                return {dest};
            paths << dest + QLatin1Char('/') + name;
        }
    }
    // Options like --relative put the copies elsewhere - then everything in the destination.
    for (const QString &path : std::as_const(paths)) {
        if (!LocalPath::info(path).exists)
            return {dest};
    }
    return paths.isEmpty() ? QStringList{dest} : paths;
}

namespace {

void dropTree(const QByteArray &path, dev_t device, RsyncJob::CacheDropResult &result, const std::atomic<bool> *cancel)
{
    if (cancel && cancel->load()) {
        result.cancelled = true;
        return;
    }
    struct stat st;
    if (::lstat(path.constData(), &st) != 0 || st.st_dev != device)
        return;
    if (S_ISREG(st.st_mode)) {
        int fd = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOATIME);
        if (fd < 0 && errno == EPERM) // O_NOATIME needs the owner of the file
            fd = ::open(path.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            ++result.errors;
            return;
        }
        ::fdatasync(fd); // normally nothing left after syncfs(); dirty pages would not be dropped
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
        ++result.files;
        result.bytes += st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        DIR *d = ::opendir(path.constData());
        if (!d)
            return;
        QList<QByteArray> children;
        while (dirent *entry = ::readdir(d)) {
            if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                children << path + '/' + entry->d_name;
        }
        ::closedir(d);
        for (const QByteArray &child : std::as_const(children)) {
            dropTree(child, device, result, cancel);
            if (result.cancelled)
                return;
        }
    }
}

} // namespace

RsyncJob::CacheDropResult RsyncJob::dropFromPageCache(const QStringList &paths, const std::atomic<bool> *cancel)
{
    CacheDropResult result;
    QList<dev_t> synced;
    for (const QString &path : paths) {
        const QByteArray encoded = LocalPath::encode(path);
        struct stat st;
        if (::lstat(encoded.constData(), &st) != 0)
            continue;
        if (!synced.contains(st.st_dev)) {
            // All dirty data of the file system at once: faster than file by file, and on a slow USB stick
            // this is where the waiting is - the data has to get there before unmounting anyway.
            const int fd = ::open(encoded.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd >= 0) {
                ::syncfs(fd);
                ::close(fd);
            }
            synced << st.st_dev;
        }
        dropTree(encoded, st.st_dev, result, cancel);
        if (result.cancelled)
            break;
    }
    return result;
}

void RsyncJob::onProcessError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || !m_process)
        return;
    emit log(LogLevel::Error, tr("Failed to start \"%1\": %2").arg(m_options.program, m_process->errorString()));
    m_process->deleteLater();
    m_process = nullptr;
    m_summary.aborted = true;
    finish();
}

int RsyncJob::removeEmptyDirectories(const QString &dir, bool keepTop, QStringList *left)
{
    const QByteArray root = LocalPath::encode(dir);
    struct stat rootStat;
    if (::lstat(root.constData(), &rootStat) != 0 || !S_ISDIR(rootStat.st_mode))
        return 0;

    // Post-order traversal: children first, then the directory itself.
    int removed = 0;
    const std::function<bool(const QByteArray &, bool)> visit = [&](const QByteArray &path, bool remove) -> bool {
        if (DIR *d = ::opendir(path.constData())) {
            QList<QByteArray> subdirs;
            while (dirent *entry = ::readdir(d)) {
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                    continue;
                const QByteArray child = path + '/' + entry->d_name;
                struct stat st;
                if (::lstat(child.constData(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    if (st.st_dev == rootStat.st_dev)
                        subdirs << child;
                    else if (left)
                        *left << LocalPath::decode(child); // a mount point - never removed
                }
            }
            ::closedir(d);
            for (const QByteArray &sub : std::as_const(subdirs))
                visit(sub, true);
        }
        if (!remove)
            return false;
        if (::rmdir(path.constData()) == 0) {
            ++removed;
            return true;
        }
        // Report only the outermost directories that stay (their parents stay anyway).
        if (left && errno != ENOENT) {
            const QString name = LocalPath::decode(path);
            left->erase(std::remove_if(left->begin(), left->end(),
                                       [&](const QString &l) { return l.startsWith(name + QLatin1Char('/')); }),
                        left->end());
            *left << name;
        }
        return false;
    };
    visit(root, !keepTop);
    return removed;
}

void RsyncJob::removeEmptiedSourceDirs(const Transfer &transfer)
{
    if (!m_options.removesSourceDirs() || transfer.sourceDirs.isEmpty())
        return;
    for (const QString &dir : transfer.sourceDirs) {
        QStringList left;
        // With a trailing "/" only the content was moved - the directory itself stays.
        const int removed = removeEmptyDirectories(dir, m_options.contentsOnly, &left);
        m_summary.sourceDirsRemoved += removed;
        if (removed > 0)
            emit log(LogLevel::Info, tr("Removed %1 emptied source director(y/ies) in %2").arg(removed).arg(LocalPath::display(dir)));
        for (const QString &path : std::as_const(left))
            emit log(LogLevel::Warning, tr("Source directory not empty - left in place: %1").arg(LocalPath::display(path)));
    }
}

void RsyncJob::finish()
{
    m_running = false;
    emit currentFileChanged(QString());
    emit statusChanged(QString());
    emit phaseChanged(QString());

    const Summary &s = m_summary;
    QString text = tr("Transfers: %1 ok, %2 with warnings, %3 failed")
                       .arg(s.transfersOk)
                       .arg(s.transfersWithWarnings)
                       .arg(s.transfersFailed);
    if (m_options.canVerify()) {
        text += tr("; verified: %1, differences: %2, verification failed: %3")
                    .arg(s.verified)
                    .arg(s.differences)
                    .arg(s.verifyFailed);
    }
    if (m_options.verifiedMove())
        text += tr("; sources removed: %1, removal failed: %2").arg(s.sourcesRemoved).arg(s.removalFailed);
    if (m_options.dryRun)
        text += tr(" (dry run - nothing was changed)");
    if (s.cancelled)
        emit log(LogLevel::Warning, tr("Cancelled by user. ") + text);
    else if (s.aborted)
        emit log(LogLevel::Error, tr("Operation aborted. ") + text);
    else if (s.ok())
        emit log(LogLevel::Success, tr("Finished successfully. ") + text);
    else
        emit log(LogLevel::Error, tr("Finished with errors. ") + text);

    if (s.ok())
        emit progress(1000);
    emit finished();
}

void RsyncJob::emitProgress(double currentFraction)
{
    const qsizetype total = m_transfers.size() * m_stepsPerTransfer;
    if (total <= 0)
        return;
    const qsizetype done = m_index * m_stepsPerTransfer + (m_phase == Phase::Verify ? 1 : m_phase == Phase::Remove ? 2 : 0);
    emit progress(int(1000.0 * (done + currentFraction) / total));
}
