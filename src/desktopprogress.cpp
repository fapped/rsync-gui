#include "desktopprogress.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>

namespace {

const QString kLauncherPath = QStringLiteral("/com/canonical/unity/launcherentry/rsync_gui");
const QString kLauncherInterface = QStringLiteral("com.canonical.Unity.LauncherEntry");
const QString kJobServer = QStringLiteral("org.kde.JobViewServer");
const QString kJobServerPath = QStringLiteral("/JobViewServer");
const QString kJobServerInterface = QStringLiteral("org.kde.JobViewServerV2");
const QString kJobInterface = QStringLiteral("org.kde.JobViewV3");

// KJob capabilities and error codes, as understood by the job view server.
constexpr int kCapabilityKillable = 0x0001;
constexpr uint kErrorNone = 0;
constexpr uint kErrorKilled = 1;
constexpr uint kErrorUserDefined = 100;

constexpr int kUpdateIntervalMs = 250;

} // namespace

DesktopProgress::DesktopProgress(QObject *parent)
    : QObject(parent)
{
    m_appUri = QStringLiteral("application://") + QGuiApplication::desktopFileName() + QStringLiteral(".desktop");
    m_updateTimer.setSingleShot(true);
    m_updateTimer.setInterval(kUpdateIntervalMs);
    connect(&m_updateTimer, &QTimer::timeout, this, &DesktopProgress::flush);
}

DesktopProgress::~DesktopProgress()
{
    if (m_active) {
        sendLauncherEntry(0.0, false);
        // Blocking: the application may be quitting, an asynchronous call would be lost.
        terminateJob(kErrorKilled, QString(), true);
    }
}

void DesktopProgress::start(const QString &title, const QString &source, const QString &destination,
                            const QString &destinationUrl)
{
    if (m_active)
        finish(true);
    m_active = true;
    m_finishPending = false;
    m_requestPending = false;
    m_jobService.clear();
    m_jobPath.clear();
    m_progress = 0.0;
    m_progressChanged = true;
    m_pending = {
        {QStringLiteral("title"), title},
        {QStringLiteral("descriptionLabel1"), tr("Source")},
        {QStringLiteral("descriptionValue1"), source},
        {QStringLiteral("descriptionLabel2"), tr("Destination")},
        {QStringLiteral("descriptionValue2"), destination},
        {QStringLiteral("percent"), 0u},
    };
    if (!destinationUrl.isEmpty())
        m_pending.insert(QStringLiteral("destUrl"), destinationUrl);
    sendLauncherEntry(0.0, true);

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return;
    QDBusMessage request = QDBusMessage::createMethodCall(kJobServer, kJobServerPath, kJobServerInterface,
                                                          QStringLiteral("requestView"));
    request << QGuiApplication::desktopFileName() << kCapabilityKillable << m_pending;
    m_requestPending = true;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *call) {
        call->deleteLater();
        m_requestPending = false;
        const QDBusPendingReply<QDBusObjectPath> reply = *call;
        if (reply.isError() || !m_active) {
            // No job view server (not Plasma) - the task manager progress still works.
            if (m_finishPending)
                m_active = false;
            return;
        }
        // The job views live on the connection that owns the server name (the reply's sender is not
        // available here), so the well-known name is used for them too.
        m_jobService = kJobServer;
        m_jobPath = reply.value().path();
        QDBusConnection::sessionBus().connect(m_jobService, m_jobPath, kJobInterface, QStringLiteral("cancelRequested"),
                                              this, SLOT(onCancelRequested()));
        if (m_finishPending) {
            terminateJob(m_finishCode, m_finishMessage, false);
            m_active = false;
            return;
        }
        flush();
    });
    // The initial properties went with the request.
    m_pending.clear();
}

void DesktopProgress::setTitle(const QString &title)
{
    if (!m_active)
        return;
    m_pending.insert(QStringLiteral("title"), title);
    scheduleUpdate();
}

void DesktopProgress::setInfo(const QString &info)
{
    if (!m_active)
        return;
    m_pending.insert(QStringLiteral("infoMessage"), info);
    scheduleUpdate();
}

void DesktopProgress::setProgress(int permille)
{
    if (!m_active)
        return;
    const double progress = qBound(0, permille, 1000) / 1000.0;
    if (progress == m_progress)
        return;
    m_progress = progress;
    m_progressChanged = true;
    m_pending.insert(QStringLiteral("percent"), uint(permille / 10));
    scheduleUpdate();
}

void DesktopProgress::finish(bool cancelled, const QString &errorMessage)
{
    if (!m_active)
        return;
    m_updateTimer.stop();
    m_pending.clear();
    sendLauncherEntry(m_progress, false);
    const uint code = cancelled ? kErrorKilled : errorMessage.isEmpty() ? kErrorNone : kErrorUserDefined;
    if (m_requestPending) {
        // The job view is still being created; it is terminated as soon as it exists.
        m_finishPending = true;
        m_finishCode = code;
        m_finishMessage = errorMessage;
        return;
    }
    terminateJob(code, errorMessage, false);
    m_active = false;
}

void DesktopProgress::onCancelRequested()
{
    emit cancelRequested();
}

void DesktopProgress::scheduleUpdate()
{
    if (!m_updateTimer.isActive())
        m_updateTimer.start();
}

void DesktopProgress::flush()
{
    if (!m_active)
        return;
    if (m_progressChanged) {
        sendLauncherEntry(m_progress, true);
        m_progressChanged = false;
    }
    if (m_jobPath.isEmpty() || m_pending.isEmpty())
        return;
    QDBusMessage update = QDBusMessage::createMethodCall(m_jobService, m_jobPath, kJobInterface, QStringLiteral("update"));
    update << m_pending;
    QDBusConnection::sessionBus().asyncCall(update);
    m_pending.clear();
}

void DesktopProgress::sendLauncherEntry(double progress, bool visible)
{
    QDBusMessage signal = QDBusMessage::createSignal(kLauncherPath, kLauncherInterface, QStringLiteral("Update"));
    signal << m_appUri
           << QVariantMap{{QStringLiteral("progress"), progress}, {QStringLiteral("progress-visible"), visible}};
    QDBusConnection::sessionBus().send(signal);
}

void DesktopProgress::terminateJob(uint errorCode, const QString &errorMessage, bool blocking)
{
    if (m_jobPath.isEmpty())
        return;
    QDBusMessage terminate = QDBusMessage::createMethodCall(m_jobService, m_jobPath, kJobInterface,
                                                            QStringLiteral("terminate"));
    terminate << errorCode << errorMessage << QVariantMap();
    if (blocking)
        QDBusConnection::sessionBus().call(terminate, QDBus::Block, 500);
    else
        QDBusConnection::sessionBus().asyncCall(terminate);
    QDBusConnection::sessionBus().disconnect(m_jobService, m_jobPath, kJobInterface, QStringLiteral("cancelRequested"),
                                             this, SLOT(onCancelRequested()));
    m_jobPath.clear();
}
