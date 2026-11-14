#include "runlog.h"

#include <QDir>
#include <QStandardPaths>

QString RunLog::defaultDirectory(const QString &appName)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QLatin1Char('/') + appName
        + QStringLiteral("/logs");
}

bool RunLog::open(const QString &directory, const QDateTime &start, QString *error)
{
    close();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QObject::tr("Cannot create the directory \"%1\".").arg(directory);
        return false;
    }
    QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString base = directory + QLatin1Char('/') + start.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    for (int n = 1; n < 1000; ++n) {
        const QString path = n == 1 ? base + QStringLiteral(".log") : QStringLiteral("%1_%2.log").arg(base).arg(n);
        m_file.setFileName(path);
        // NewOnly: another instance starting in the same second takes the next name.
        if (m_file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text)) {
            m_file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            return true;
        }
        if (QFile::exists(path))
            continue;
        if (error)
            *error = QObject::tr("Cannot create \"%1\": %2").arg(path, m_file.errorString());
        return false;
    }
    if (error)
        *error = QObject::tr("Cannot create a log file in \"%1\".").arg(directory);
    return false;
}

void RunLog::write(const QDateTime &time, const QString &text)
{
    if (!m_file.isOpen())
        return;
    const QString stamp = time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss "));
    QByteArray data;
    for (const QString &line : text.split(QLatin1Char('\n')))
        data += (stamp + line + QLatin1Char('\n')).toUtf8();
    m_file.write(data);
    m_file.flush();
}

void RunLog::close()
{
    if (m_file.isOpen())
        m_file.close();
}
