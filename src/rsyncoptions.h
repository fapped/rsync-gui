#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

class QSettings;

// The supported subset of rsync options. Everything else can be passed in `extraArguments`.
struct RsyncOptions
{
    enum class Progress {
        None,
        PerFile, // --progress (-P together with --partial)
        Overall, // --info=progress2
    };

    enum class Existing {
        Replace,        // default: transfer files that differ (size and time, or checksum with -c)
        SkipNewer,      // -u, --update: skip files that are newer in the destination
        IgnoreExisting, // --ignore-existing: never touch files that already exist in the destination
    };

    static constexpr int JsonFormatVersion = 1;

    QString program = QStringLiteral("rsync");
    bool archive = true;       // -a
    bool recursive = true;     // -r (implied by -a, used only without it)
    bool hardLinks = false;    // -H
    bool acls = false;         // -A
    bool xattrs = false;       // -X
    bool verbose = true;       // -v
    bool human = true;         // -h
    bool partial = true;       // --partial
    Progress progress = Progress::PerFile;
    bool checksum = false;     // -c
    Existing existing = Existing::Replace;
    bool compress = false;     // -z
    bool contentsOnly = false; // trailing "/" on local source directories
    bool deleteExtraneous = false; // --delete
    bool removeSourceFiles = false; // --remove-source-files
    bool dryRun = false;       // -n
    bool verifyAfter = false;  // second pass comparing checksums (not an rsync option)
    bool useBwLimit = false;   // --bwlimit
    QString bwLimit;
    bool useRemoteShell = false; // -e
    QString remoteShell;
    QString extraArguments;    // split like a shell command line

    // The options for a copy. `port` (if non-zero) is added to the remote shell command.
    QStringList arguments(int port = 0) const;
    // The options of the verification pass: a dry run that lists files whose content
    // differs (--checksum --itemize-changes), without changing anything.
    QStringList verifyArguments(int port = 0) const;
    QStringList remoteShellArguments(int port) const;
    QStringList extraArgumentList() const;

    // Whether the verification pass is meaningful for these options.
    bool canVerify() const { return verifyAfter && !dryRun && !removeSourceFiles; }

    // Returns an empty string if the options are valid, otherwise an error description.
    QString validate() const;

    void save(QSettings &settings) const;
    static RsyncOptions load(QSettings &settings);

    // Importable/exportable JSON configuration. Keys missing from the JSON keep the values from `base`.
    QJsonObject toJson() const;
    static RsyncOptions fromJson(const QJsonObject &json, QString *error = nullptr);
    static RsyncOptions fromJson(const QJsonObject &json, QString *error, const RsyncOptions &base);
    bool saveToFile(const QString &path, QString *error = nullptr) const;
    static RsyncOptions loadFromFile(const QString &path, QString *error = nullptr);
    static RsyncOptions loadFromFile(const QString &path, QString *error, const RsyncOptions &base,
                                     QString *name = nullptr);

    // Presets are JSON settings files in <data dir>/rsync-gui/presets/<name>.json.
    // Earlier directories take precedence.
    static QStringList presetSearchPaths();
    // Returns the file of the given preset or an empty string. A name containing '/' is treated as a path.
    static QString locatePreset(const QString &name, const QStringList &searchPaths = presetSearchPaths());
    static QMap<QString, QString> availablePresets(const QStringList &searchPaths = presetSearchPaths());

    static bool isValidBwLimit(const QString &limit);
    static QString progressName(Progress progress);
    static bool progressFromName(const QString &name, Progress *progress);
    static QString existingName(Existing existing);
    static bool existingFromName(const QString &name, Existing *existing);

    bool operator==(const RsyncOptions &other) const;
    bool operator!=(const RsyncOptions &other) const { return !(*this == other); }
};

QString shellQuote(const QString &arg);
QString commandLine(const QString &program, const QStringList &args);
// Splits a command line like a POSIX shell (quotes and backslashes); returns false on unbalanced quotes.
bool splitArguments(const QString &text, QStringList *out);
