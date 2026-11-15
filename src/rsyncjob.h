#pragma once

#include "location.h"
#include "rsyncoptions.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>

#include <atomic>
#include <memory>

// Copies sources to a destination with rsync, optionally followed by a verification pass.
// rsync can read from only one remote host per run, so the sources are grouped into transfers
// (all local sources, and one transfer per remote origin) run one after another.
class QThread;

class RsyncJob : public QObject
{
    Q_OBJECT

public:
    enum class LogLevel { Info, Output, Warning, Error, Success };
    Q_ENUM(LogLevel)

    // One rsync process (plus one for the verification pass).
    struct Transfer
    {
        QStringList sources; // rsync arguments (local paths or user@host:path; the host's paths with `host`)
        QString destination; // rsync argument, always ending with "/"
        int port = 0;        // ssh port for the remote side (or of `host`), 0 = default
        QString description; // "local files" or the remote origin, for the log
        QStringList sourceDirs; // local source directories (for removing them after a move)
        // Non-empty ("[user@]host"): the sources and the destination are on this host and rsync runs there,
        // started over ssh - the data does not go through this computer.
        QString host;
        // On the server: the destination is created first (mkdir -p), as the window does for local ones.
        bool createDestination = false;
        // Options for this transfer only, after those of RsyncOptions (copy and removal passes), e.g. the --chmod of
        // a network share reached over ssh.
        QStringList extraArguments;

        bool runsOnServer() const { return !host.isEmpty(); }
    };

    // The runs of a transfer: the copy, the verification, the removal of the sources of a verified move, and for a
    // move on the server the removal of the emptied source directories there (not rsync: a sh script over ssh).
    enum class Pass { Copy, Verify, Remove, CleanUp };

    // The process to start for one run of a transfer.
    struct Command
    {
        QString program;
        QStringList arguments;
        // For the log and the preview: a command line for a local shell. The remote command of a transfer
        // on the server is shown in double quotes when that is easier to read than '...'\''...'.
        QString display() const;
    };
    // The local rsync, or for a transfer that runs on the server: ssh [-p N] -- host 'rsync ... -- SRC DST/'.
    // With `reportPid` the remote command first prints "RSYNC-GUI-PID <pid>" (the PID of the remote rsync,
    // which replaces the shell with exec), so that cancelling can stop it (see cancel()).
    static Command command(const Transfer &transfer, const RsyncOptions &options, Pass pass, bool reportPid = false);
    // QProcess passes arguments as UTF-8: for arguments with bytes that are not valid UTF-8 (see LocalPath) the
    // child execs `program` itself with the exact bytes.
    static void setRawArguments(QProcess *process, const QString &program, const QStringList &args);
    // The options of the rsync of a pass (without the sources and the destination).
    static QStringList rsyncArguments(const Transfer &transfer, const RsyncOptions &options, Pass pass);
    // The first line printed by a command with `reportPid`.
    static constexpr const char *PidLinePrefix = "RSYNC-GUI-PID ";

    struct Plan
    {
        QList<Transfer> transfers;
        QStringList missing; // local sources that do not exist (left out of the transfers)
        QStringList notes;   // duplicates etc., informational
        QString error;       // non-empty if nothing can be transferred

        bool ok() const { return error.isEmpty() && !transfers.isEmpty(); }
    };

    struct ProgressInfo
    {
        QString transferred; // as printed by rsync, e.g. "3,00M" or "1,048,576"
        int percent = 0;
        QString rate;
        QString eta;
        int remaining = -1;  // files left to check, -1 if not reported in this line
        int total = -1;
        bool totalFinal = false; // "to-chk" (final) rather than "ir-chk" (the file list still grows)
        int transferNumber = -1; // "xfr#N": set on the last line of a transferred file
        qint64 bytes = -1;       // `transferred` in bytes (-1 if it cannot be parsed)
        qint64 bytesPerSecond = -1; // `rate` in bytes per second
    };

    // The entries of the copy passes so far (all transfers of the job), from rsync's progress output
    // ("xfr#N, to-chk=R/T"). rsync counts every entry of the file list (directories too), also those it
    // skips because they are up to date. With incremental recursion (the default) the file list is still
    // growing and `total` is not final yet; neither is it while more transfers follow.
    struct FileCount
    {
        qint64 checked = 0;
        qint64 total = 0;
        bool totalFinal = false;
        qint64 transferred = 0; // the files rsync actually copied

        bool isEmpty() const { return total == 0 && checked == 0 && transferred == 0; }
        bool operator==(const FileCount &other) const
        {
            return checked == other.checked && total == other.total && totalFinal == other.totalFinal
                && transferred == other.transferred;
        }
    };
    // E.g. "Checked: 1,200 / 5,000+ (3,800+ left) · transferred: 340"; empty if nothing has been reported.
    static QString fileCountText(const FileCount &count);

    struct Summary
    {
        int transfersOk = 0;
        int transfersWithWarnings = 0; // e.g. source files vanished during the transfer
        int transfersFailed = 0;
        int verified = 0;
        int verifyFailed = 0;
        int differences = 0; // entries that differ after copying (verification pass)
        int sourceDirsRemoved = 0; // emptied source directories removed after a move
        int sourcesRemoved = 0; // verified move: transfers whose sources were removed after the verification
        int removalFailed = 0;
        int warnings = 0; // warnings and errors in the log of the job (rsync's, vanished files, directories left...)
        bool cancelled = false;
        bool aborted = false; // e.g. rsync could not be started

        bool ok() const
        {
            return !cancelled && !aborted && transfersFailed == 0 && verifyFailed == 0 && differences == 0
                && removalFailed == 0;
        }
        // Everything done without a single warning: e.g. for closing the window when finished.
        bool clean() const { return ok() && transfersWithWarnings == 0 && warnings == 0; }
    };

    static Plan plan(const QList<Location> &sources, const Location &destination, const RsyncOptions &options);
    // The total of "N files to consider" (--info=flist2), -1 for other lines.
    static int parseFileListTotal(const QString &line);
    // The name of the thread that writes the copies to the disk before a verification (see startVerify()).
    static constexpr const char *CacheDropThreadName = "rsync-gui-cache";
    // Parses an rsync --progress / --info=progress2 line.
    // `humanLevel` is the number of -h options + 1, as in rsync: 1 = plain numbers with thousands
    // separators, 2 = units of 1000 ("4,62M"), 3 = units of 1024. The rate is always in units of 1024.
    static bool parseProgressLine(const QString &line, ProgressInfo *info, int humanLevel = 1);
    static qint64 parseSize(const QString &text, int humanLevel);
    static qint64 parseRate(const QString &text);
    // The -h level of an rsync command line (see parseProgressLine()).
    static int humanLevel(const QStringList &args);
    // Returns true if the line (from the verification pass, "--out-format=%i %n%L") reports an entry
    // that differs or is missing in the destination; `name` receives the entry name.
    static bool parseDifferenceLine(const QString &line, QString *name = nullptr);
    static QString exitCodeDescription(int code);
    // The --modify-window a local destination needs: 1 if its file system keeps file times less precisely
    // than to the second (FAT: 2 s - without it every file with an odd second is copied again on every run),
    // else 0. Tries it: a temporary file in the destination (or its nearest existing parent) gets a time
    // with an odd second, and the time read back is compared. Without `mayWrite` (a dry run) or when the
    // file cannot be created, the file system type decides (FAT).
    static int modifyWindowFor(const QString &localDestination, bool mayWrite = true);
    struct CacheDropResult
    {
        qint64 files = 0;
        qint64 bytes = 0;
        int errors = 0; // files that could not be opened
        bool cancelled = false;
    };
    // Writes the cached data of the file systems of `paths` to the disk (syncfs) and drops the regular files
    // under `paths` (recursively, without following symlinks or entering other file systems) from the page
    // cache, so that reading them again reads the disk. Only clean pages are dropped - no data is lost.
    static CacheDropResult dropFromPageCache(const QStringList &paths, const std::atomic<bool> *cancel = nullptr);
    // The paths in the local destination of a transfer that the verification reads: the copies of the
    // sources ("dir" -> dest/dir, "dir/" -> the entries of dir in dest). The destination itself if that
    // cannot be told (remote "dir/" sources, a copy that is not where expected).
    static QStringList destinationPaths(const Transfer &transfer);
    // Whether the file system of an existing path is FAT (vfat / msdos) - by the type only, nothing is written.
    static bool isFatFileSystem(const QString &path);
    // Removes the empty directories in `dir` (deepest first) and `dir` itself unless `keepTop`.
    // Only rmdir() is used, so a directory that is not empty is never touched; symlinks are not
    // followed and other file systems are not entered. Returns the number of removed directories;
    // `left` receives the directories that stay because they are not empty (or cannot be removed).
    static int removeEmptyDirectories(const QString &dir, bool keepTop, QStringList *left = nullptr);

    RsyncJob(QList<Transfer> transfers, RsyncOptions options, QObject *parent = nullptr);
    ~RsyncJob() override;

    void start();
    void cancel();
    bool isRunning() const { return m_running; }
    const Summary &summary() const { return m_summary; }
    const QList<Transfer> &transfers() const { return m_transfers; }
    const RsyncOptions &options() const { return m_options; }
    // Entries reported by the verification pass.
    const QStringList &differingFiles() const { return m_differingFiles; }

signals:
    void log(RsyncJob::LogLevel level, const QString &text);
    void progress(int permille);
    void statusChanged(const QString &text); // speed, ETA etc. of the running transfer
    void currentFileChanged(const QString &path);
    void filesChanged(const RsyncJob::FileCount &count);
    // Speed and the bytes copied so far (all transfers of the job), from rsync's progress output.
    void transferRateChanged(qint64 bytesPerSecond, qint64 processedBytes);
    void phaseChanged(const QString &text);
    void finished();

private:
    using Phase = Pass; // Remove: the last pass of a verified move

    void startNext();
    void startProcess();
    // Before the verification of a local destination: drops the copies from the page cache (in a thread).
    void startVerify();
    void onCacheDropped(const CacheDropResult &result);
    // Stops the rsync of a transfer that runs on the server: ssh host kill -TERM <pid> (detached).
    void killRemote();
    void onReadyRead();
    void handleLine(const QString &line);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void removeEmptiedSourceDirs(const Transfer &transfer);
    // For a move on the server: starts the CleanUp pass; false if there is nothing to do.
    bool startRemoteCleanUp();
    void onCleanUpFinished(int exitCode, QProcess::ExitStatus status);
    void finish();
    void emitProgress(double currentFraction);

    QList<Transfer> m_transfers;
    RsyncOptions m_options;
    Summary m_summary;
    QStringList m_differingFiles;
    QPointer<QProcess> m_process;
    QByteArray m_pending;
    qsizetype m_index = 0;
    Phase m_phase = Phase::Copy;
    int m_stepsPerTransfer = 1;
    int m_transferDifferences = 0;
    int m_humanLevel = 1;
    qint64 m_bytesBefore = 0;    // bytes of the previous transfers of the job
    qint64 m_completedBytes = 0; // per-file progress: bytes of the files finished in this transfer
    qint64 m_processedBytes = 0; // this transfer
    int m_lastTransferNumber = 0;
    int m_lastDone = 0;  // files checked so far, from the last "to-chk" report
    int m_lastTotal = 0;
    bool m_lastTotalFinal = false;
    int m_lastTransferred = 0; // the highest "xfr#N" of the running copy pass
    FileCount m_filesBefore;   // the finished copy passes
    bool m_running = false;
    bool m_cancelRequested = false;
    bool m_awaitingPid = false; // a transfer on the server: the first line is the PID of the remote rsync
    qint64 m_remotePid = 0;
    QByteArray m_cleanUpOutput; // the records of the CleanUp pass
    bool m_shellNotClean = false; // rsync reported "protocol version mismatch -- is your shell clean?"
    std::shared_ptr<std::atomic<bool>> m_dropCancel; // set while the page cache is being dropped
    QPointer<QThread> m_dropThread;
};
