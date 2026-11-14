#pragma once

#include <QDateTime>
#include <QFile>
#include <QString>

// The log of one run written to a file: <directory>/<yyyy-MM-dd_HH-mm-ss>.log (with "_2", "_3", ...
// when two runs start in the same second). The log names the processed files, so the directory and
// the file are readable by the user only. Every line starts with the date and time.
class RunLog
{
public:
    // <XDG_STATE_HOME, usually ~/.local/state>/<appName>/logs
    static QString defaultDirectory(const QString &appName);

    bool open(const QString &directory, const QDateTime &start, QString *error = nullptr);
    bool isOpen() const { return m_file.isOpen(); }
    QString path() const { return m_file.fileName(); }
    // Writes every line of `text` (flushed right away - the log is complete even if the process dies).
    void write(const QDateTime &time, const QString &text);
    void close();

private:
    QFile m_file;
};
