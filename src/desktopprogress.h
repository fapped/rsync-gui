#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

// Shows the progress of a transfer outside the window, over D-Bus:
// - on the task manager entry (com.canonical.Unity.LauncherEntry, supported by Plasma and others),
// - as a job in the notification area (org.kde.JobViewServerV2, Plasma), with a cancel button.
// Without these services (other desktops, no session bus) everything is a no-op.
class DesktopProgress : public QObject
{
    Q_OBJECT

public:
    explicit DesktopProgress(QObject *parent = nullptr);
    ~DesktopProgress() override; // an unfinished job is reported as cancelled

    // `destinationUrl` is offered by Plasma as "Open destination" after the job.
    void start(const QString &title, const QString &source, const QString &destination, const QString &destinationUrl);
    void setTitle(const QString &title);
    void setInfo(const QString &info);
    void setProgress(int permille);
    // Numeric speed and amount - Plasma draws the speed graph of the job from them.
    void setSpeed(qint64 bytesPerSecond, qint64 processedBytes);
    // Hides the progress and ends the job (a non-empty error message marks it as failed).
    void finish(bool cancelled, const QString &errorMessage = QString());
    bool isActive() const { return m_active; }

signals:
    void cancelRequested();

private slots:
    void onCancelRequested();

private:
    void scheduleUpdate();
    void flush();
    void sendLauncherEntry(double progress, bool visible);
    void terminateJob(uint errorCode, const QString &errorMessage, bool blocking);

    QString m_appUri;       // application://rsync-gui.desktop
    QString m_jobService;   // the owner of the job view, known after requestView() returns
    QString m_jobPath;
    QVariantMap m_pending;  // job properties not sent yet (updates are coalesced)
    QTimer m_updateTimer;
    double m_progress = 0.0;
    bool m_progressChanged = false;
    bool m_active = false;
    bool m_requestPending = false; // requestView() has not returned yet
    bool m_finishPending = false; // finish() was called before the job view was created
    uint m_finishCode = 0;
    QString m_finishMessage;
};
