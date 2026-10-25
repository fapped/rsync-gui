#pragma once

#include "location.h"
#include "rsyncoptions.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>

// Copies sources to a destination with rsync, optionally followed by a verification pass.
// rsync can read from only one remote host per run, so the sources are grouped into transfers
// (all local sources, and one transfer per remote origin) run one after another.
class RsyncJob : public QObject
{
    Q_OBJECT

public:
    enum class LogLevel { Info, Output, Warning, Error, Success };
    Q_ENUM(LogLevel)

    // One rsync process (plus one for the verification pass).
    struct Transfer
    {
        QStringList sources; // rsync arguments (local paths or user@host:path)
        QString destination; // rsync argument, always ending with "/"
        int port = 0;        // ssh port for the remote side, 0 = default
        QString description; // "local files" or the remote origin, for the log
    };

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
    };

    struct Summary
    {
        int transfersOk = 0;
        int transfersWithWarnings = 0; // e.g. source files vanished during the transfer
        int transfersFailed = 0;
        int verified = 0;
        int verifyFailed = 0;
        int differences = 0; // entries that differ after copying (verification pass)
        bool cancelled = false;
        bool aborted = false; // e.g. rsync could not be started

        bool ok() const
        {
            return !cancelled && !aborted && transfersFailed == 0 && verifyFailed == 0 && differences == 0;
        }
    };

    static Plan plan(const QList<Location> &sources, const Location &destination, const RsyncOptions &options);
    // Parses an rsync --progress / --info=progress2 line.
    static bool parseProgressLine(const QString &line, ProgressInfo *info);
    // Returns true if the line (from the verification pass, "--out-format=%i %n%L") reports an entry
    // that differs or is missing in the destination; `name` receives the entry name.
    static bool parseDifferenceLine(const QString &line, QString *name = nullptr);
    static QString exitCodeDescription(int code);

    RsyncJob(QList<Transfer> transfers, RsyncOptions options, QObject *parent = nullptr);
    ~RsyncJob() override;

    void start();
    void cancel();
    bool isRunning() const { return m_running; }
    const Summary &summary() const { return m_summary; }
    const QList<Transfer> &transfers() const { return m_transfers; }
    // Entries reported by the verification pass.
    const QStringList &differingFiles() const { return m_differingFiles; }

signals:
    void log(RsyncJob::LogLevel level, const QString &text);
    void progress(int permille);
    void statusChanged(const QString &text); // speed, ETA etc. of the running transfer
    void currentFileChanged(const QString &path);
    void phaseChanged(const QString &text);
    void finished();

private:
    enum class Phase { Copy, Verify };

    void startNext();
    void startProcess();
    void onReadyRead();
    void handleLine(const QString &line);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
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
    int m_lastDone = 0;  // files checked so far, from the last "to-chk" report
    int m_lastTotal = 0;
    bool m_running = false;
    bool m_cancelRequested = false;
};
