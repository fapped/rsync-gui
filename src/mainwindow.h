#pragma once

#include "location.h"
#include "rsyncjob.h"
#include "rsyncoptions.h"

#include <QMainWindow>
#include <QPointer>

class QAction;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMimeData;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
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
    void browseFiles();
    void browseDirectory();
    void browseProgram();
    void importSettings();
    void exportSettings();
    void showAbout();
    void startCopy();
    void cancelCopy();
    void appendLog(RsyncJob::LogLevel level, const QString &text);
    void onJobFinished();

    QString settingsDialogDir() const;
    void rememberSettingsDialogDir(const QString &file) const;
    static QStringList pathsFromMimeData(const QMimeData *mime);

    QListWidget *m_sourceList = nullptr;
    QLineEdit *m_sourceEdit = nullptr;
    QLabel *m_sourceInfo = nullptr;
    QPushButton *m_removeSourcesButton = nullptr;
    QPushButton *m_clearSourcesButton = nullptr;
    QGroupBox *m_sourceBox = nullptr;

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
    QCheckBox *m_contentsCheck = nullptr;
    QCheckBox *m_deleteCheck = nullptr;
    QCheckBox *m_removeSourceCheck = nullptr;
    QCheckBox *m_dryRunCheck = nullptr;
    QCheckBox *m_verifyCheck = nullptr;
    QLineEdit *m_extraEdit = nullptr;
    QAction *m_importAction = nullptr;
    QAction *m_exportAction = nullptr;
    QMenu *m_presetMenu = nullptr;

    QLineEdit *m_previewEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QCheckBox *m_closeWhenFinishedCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_currentFile = nullptr;
    QLabel *m_transferStatus = nullptr;
    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QPointer<RsyncJob> m_job;
    // False while the options come from a preset or a --config file: they are not remembered.
    bool m_rememberOptions = true;
};
