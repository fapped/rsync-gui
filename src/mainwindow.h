#pragma once

#include "desktopprogress.h"
#include "location.h"
#include "remoteprobe.h"
#include "rsyncjob.h"
#include "runlog.h"
#include "rsyncoptions.h"

#include <QDate>
#include <QHash>
#include <QMainWindow>
#include <QPointer>

#include <atomic>
#include <memory>
#include <optional>

class QAction;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QSplitter;
class ElidedLabel;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMimeData;
class QPlainTextEdit;
class QPushButton;
class SpeedGraph;
class QTabWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // Whether options loaded from a file become the remembered options for the next runs.
    enum class Persistence { Remember, SessionOnly };

    explicit MainWindow(QWidget *parent = nullptr);

    void addSources(const QStringList &paths);
    void setDestination(const QString &destination);
    // Opens the directory dialog for choosing the destination.
    void browseDestination();
    // Loads options from a JSON settings file; shows an error dialog and returns false on failure.
    bool importSettingsFile(const QString &path, Persistence persistence = Persistence::Remember);
    // Loads a preset by name (see RsyncOptions::locatePreset) for this session only.
    bool loadPreset(const QString &name);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void buildMenus();
    void populatePresetMenu();
    void populateRecentSettingsMenu();
    // Adds a settings file to File > Recent settings (presets are not added).
    void rememberRecentSettingsFile(const QString &file);
    void loadSettings();
    void saveSettings() const;
    void setOptionsSource(const QString &description);

    RsyncOptions currentOptions() const;
    void applyOptions(const RsyncOptions &options);
    // The boxes of -rlptgoD (the parts of -a), in this order.
    QList<QCheckBox *> impliedByArchive() const;
    void onArchiveToggled(bool on);
    QList<Location> sources() const;
    Location destination() const;
    void refreshSourceItem(QListWidgetItem *item);
    void refreshSources();
    void updateSourceInfo();
    void updateDestinationInfo();
    void updatePreview();
    void updateControls();
    void rememberDestination(const QString &destination);

    void addTypedSource();
    void pasteSources();
    void removeSelectedSources();
    // `builtIn`: the built-in file dialog, otherwise the preferred one.
    void browseFiles(bool builtIn = false);
    void browseDirectory(bool builtIn = false);
    void browseProgram();
    void importSettings();
    void exportSettings();
    void showAbout();
    void startCopy();
    // Transfers that run on a server: checks the servers first (asynchronously, see RemoteProbe), then continues
    // with continueCopy(). Returns false if there is nothing to check.
    bool checkServers(const RsyncOptions &options, const Location &dest, RsyncJob::Plan plan);
    void onServersChecked();
    // The rest of startCopy() after the checks: FAT, the confirmation of deleting options, the start.
    void continueCopy(RsyncOptions options, const Location &dest, RsyncJob::Plan plan, const QStringList &infos);
    bool isBusy() const; // a job runs or the servers are being checked
    void cancelCopy();
    void startDesktopProgress(const RsyncOptions &options, const Location &dest);
    void measureSourcesForDesktopProgress(const RsyncOptions &options);
    void stopMeasuringSources();
    void appendLog(RsyncJob::LogLevel level, const QString &text);
    // `withDate`: the line starts with the date too (the start of a run).
    void appendLogLines(RsyncJob::LogLevel level, const QString &text, bool withDate);
    void onJobFinished();

    QString settingsDialogDir() const;
    void rememberSettingsDialogDir(const QString &file) const;
    static QStringList pathsFromMimeData(const QMimeData *mime);
    // Paths with U+FFFD from a file manager that match several names: the user chooses in the built-in dialog.
    void chooseReplacedBytesMatches(const QList<QPair<QString, QStringList>> &paths);

    QListWidget *m_sourceList = nullptr;
    QLineEdit *m_sourceEdit = nullptr;
    QLabel *m_sourceInfo = nullptr;
    QPushButton *m_removeSourcesButton = nullptr;
    QPushButton *m_clearSourcesButton = nullptr;
    QGroupBox *m_sourceBox = nullptr;
    QSplitter *m_splitter = nullptr; // sources | destination + options/log tabs, the user drags the handle

    QGroupBox *m_destinationBox = nullptr;
    QComboBox *m_destinationCombo = nullptr;
    QLabel *m_destinationInfo = nullptr;

    QGroupBox *m_optionsBox = nullptr;
    QComboBox *m_programCombo = nullptr;
    QCheckBox *m_archiveCheck = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    QCheckBox *m_linksCheck = nullptr;
    QCheckBox *m_permsCheck = nullptr;
    QCheckBox *m_timesCheck = nullptr;
    QCheckBox *m_groupCheck = nullptr;
    QCheckBox *m_ownerCheck = nullptr;
    QCheckBox *m_devicesCheck = nullptr;
    // The user's choice of -rlptgoD while archive mode is on (the boxes then show -a's values).
    QList<bool> m_impliedValues = QList<bool>(7, true);
    QCheckBox *m_hardLinksCheck = nullptr;
    QCheckBox *m_aclsCheck = nullptr;
    QCheckBox *m_xattrsCheck = nullptr;
    QComboBox *m_existingCombo = nullptr;
    QCheckBox *m_checksumCheck = nullptr;
    QComboBox *m_progressCombo = nullptr;
    QCheckBox *m_partialCheck = nullptr;
    QCheckBox *m_verboseCheck = nullptr;
    QCheckBox *m_humanCheck = nullptr;
    QCheckBox *m_compressCheck = nullptr;
    QCheckBox *m_bwLimitCheck = nullptr;
    QLineEdit *m_bwLimitEdit = nullptr;
    QCheckBox *m_remoteShellCheck = nullptr;
    QLineEdit *m_remoteShellEdit = nullptr;
    QCheckBox *m_runOnServerCheck = nullptr;
    QCheckBox *m_contentsCheck = nullptr;
    QCheckBox *m_deleteCheck = nullptr;
    QCheckBox *m_removeSourceCheck = nullptr;
    QCheckBox *m_removeSourceDirsCheck = nullptr;
    QCheckBox *m_dryRunCheck = nullptr;
    QCheckBox *m_verifyCheck = nullptr;
    QCheckBox *m_autoModifyWindowCheck = nullptr;
    QLineEdit *m_extraEdit = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_exportAction = nullptr;
    QMenu *m_presetMenu = nullptr;
    QMenu *m_recentSettingsMenu = nullptr;

    QLineEdit *m_previewEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_quitButton = nullptr;
    QCheckBox *m_closeWhenFinishedCheck = nullptr;
    SpeedGraph *m_progress = nullptr; // the progress bar with the speed graph
    ElidedLabel *m_currentFile = nullptr;
    ElidedLabel *m_transferStatus = nullptr;
    QLabel *m_fileCount = nullptr;   // in the status bar
    QLabel *m_resultLabel = nullptr; // the result of the last run, in the status bar
    QString m_transferStatusText; // the last status of the job, for the desktop notification
    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QCheckBox *m_logToFileCheck = nullptr; // RsyncOptions::logToFile
    RunLog m_runLog; // open while a run with logToFile goes on

    QPointer<RsyncJob> m_job;
    // A copy waiting for the checks of its servers.
    struct PendingCopy
    {
        RsyncOptions options;
        Location dest;
        RsyncJob::Plan plan;
        QHash<int, RemoteProbe::Result> results; // by the index of the transfer
        QList<QPointer<RemoteProbe>> probes;
    };
    std::optional<PendingCopy> m_pending;
    QPointer<QProcess> m_remoteMeasure; // du on the server for the total size
    QDate m_logDate; // the date of the last log line - a new day gets a line of its own
    DesktopProgress *m_desktopProgress = nullptr;
    std::shared_ptr<std::atomic_bool> m_measureCancel; // stops measuring the sources
    // False while the options come from a preset or a --config file: they are not remembered.
    bool m_rememberOptions = true;
};
