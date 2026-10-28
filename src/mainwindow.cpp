#include "mainwindow.h"
#include "localpath.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTabWidget>
#include <QtConcurrent/QtConcurrentRun>
#include <QDateTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kLocationRole = Qt::UserRole;
constexpr int kMaxRecentDestinations = 15;
constexpr int kMaxRecentSettings = 10;
const QString kRecentSettingsKey = QStringLiteral("paths/recentSettings");

QString settingsFileFilter()
{
    return MainWindow::tr("Rsync GUI settings (*.json);;All files (*)");
}

QString optionsBoxTitle()
{
    return MainWindow::tr("rsync options");
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Rsync GUI"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("folder-sync")));
    setAcceptDrops(true);
    m_desktopProgress = new DesktopProgress(this);
    connect(m_desktopProgress, &DesktopProgress::cancelRequested, this, &MainWindow::cancelCopy);
    buildUi();
    buildMenus();
    loadSettings();
    updateSourceInfo();
    updateDestinationInfo();
    updatePreview();
    updateControls();
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);

    // --- Sources ---
    m_sourceBox = new QGroupBox(tr("Sources"), central);
    auto *sourceLayout = new QVBoxLayout(m_sourceBox);
    m_sourceList = new QListWidget(m_sourceBox);
    m_sourceList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_sourceList->setAcceptDrops(false); // drops are handled by the window (URL to path conversion)
    m_sourceList->setMinimumHeight(70);
    m_sourceList->setMaximumHeight(150); // the options need the space more; the list scrolls
    m_sourceList->setContextMenuPolicy(Qt::ActionsContextMenu);
    sourceLayout->addWidget(m_sourceList, 1);

    auto *pasteAction = new QAction(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste paths"), m_sourceList);
    pasteAction->setShortcut(QKeySequence::Paste);
    pasteAction->setShortcutContext(Qt::WidgetShortcut);
    auto *removeAction = new QAction(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove from list"), m_sourceList);
    removeAction->setShortcut(QKeySequence::Delete);
    removeAction->setShortcutContext(Qt::WidgetShortcut);
    m_sourceList->addAction(pasteAction);
    m_sourceList->addAction(removeAction);

    auto *pathRow = new QHBoxLayout;
    m_sourceEdit = new QLineEdit(m_sourceBox);
    m_sourceEdit->setPlaceholderText(tr("Type or paste a path (also user@host:/path or sftp://...) and press Enter"));
    m_sourceEdit->setClearButtonEnabled(true);
    m_sourceEdit->setAcceptDrops(false);
    auto *addButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), m_sourceBox);
    pathRow->addWidget(m_sourceEdit, 1);
    pathRow->addWidget(addButton);
    sourceLayout->addLayout(pathRow);

    auto *sourceButtons = new QHBoxLayout;
    auto *addFilesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), tr("Add files..."), m_sourceBox);
    auto *addDirButton = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Add directory..."), m_sourceBox);
    auto *pasteButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-paste")), tr("Paste"), m_sourceBox);
    pasteButton->setToolTip(tr("Add paths from the clipboard (one per line, or files copied in a file manager)"));
    m_removeSourcesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), m_sourceBox);
    m_clearSourcesButton = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-clear-list")), tr("Clear"), m_sourceBox);
    sourceButtons->addWidget(addFilesButton);
    sourceButtons->addWidget(addDirButton);
    sourceButtons->addWidget(pasteButton);
    sourceButtons->addStretch(1);
    sourceButtons->addWidget(m_removeSourcesButton);
    sourceButtons->addWidget(m_clearSourcesButton);
    sourceLayout->addLayout(sourceButtons);

    m_sourceInfo = new QLabel(m_sourceBox);
    m_sourceInfo->setWordWrap(true);
    sourceLayout->addWidget(m_sourceInfo);
    mainLayout->addWidget(m_sourceBox);

    // --- Destination ---
    m_destinationBox = new QGroupBox(tr("Destination directory"), central);
    auto *destLayout = new QVBoxLayout(m_destinationBox);
    auto *destRow = new QHBoxLayout;
    m_destinationCombo = new QComboBox(m_destinationBox);
    m_destinationCombo->setEditable(true);
    m_destinationCombo->setInsertPolicy(QComboBox::NoInsert);
    m_destinationCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_destinationCombo->setMinimumContentsLength(30);
    m_destinationCombo->lineEdit()->setPlaceholderText(tr("Local directory, user@host:/path or sftp://host/path"));
    m_destinationCombo->lineEdit()->setClearButtonEnabled(true);
    m_destinationCombo->setToolTip(tr("The sources are copied into this directory. Recently used destinations "
                                      "are available in the drop-down list."));
    auto *destBrowse = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Browse..."), m_destinationBox);
    destRow->addWidget(m_destinationCombo, 1);
    destRow->addWidget(destBrowse);
    destLayout->addLayout(destRow);
    m_destinationInfo = new QLabel(m_destinationBox);
    m_destinationInfo->setWordWrap(true);
    destLayout->addWidget(m_destinationInfo);
    mainLayout->addWidget(m_destinationBox);

    // --- Options ---
    m_optionsBox = new QGroupBox(optionsBoxTitle());
    auto *form = new QFormLayout(m_optionsBox);

    auto *programRow = new QHBoxLayout;
    m_programCombo = new QComboBox(m_optionsBox);
    m_programCombo->setEditable(true);
    m_programCombo->setInsertPolicy(QComboBox::NoInsert);
    if (!QStandardPaths::findExecutable(QStringLiteral("rsync")).isEmpty())
        m_programCombo->addItem(QStringLiteral("rsync"));
    auto *programBrowse = new QPushButton(tr("..."), m_optionsBox);
    programBrowse->setToolTip(tr("Select the rsync executable"));
    programRow->addWidget(m_programCombo, 1);
    programRow->addWidget(programBrowse);
    form->addRow(tr("Program:"), programRow);

    auto *preserveGrid = new QGridLayout;
    m_archiveCheck = new QCheckBox(tr("Archive mode (-a)"), m_optionsBox);
    m_archiveCheck->setToolTip(tr("-a, --archive: recursive, preserve symlinks, permissions, times, group, owner "
                                  "and special files (-rlptgoD).\nTurn it off to choose the parts separately, e.g. -rlt "
                                  "for FAT/exFAT/NTFS drives or a NAS without permissions and owners."));
    m_recursiveCheck = new QCheckBox(tr("Recursive (-r)"), m_optionsBox);
    m_recursiveCheck->setToolTip(tr("-r, --recursive: copy directories recursively"));
    m_linksCheck = new QCheckBox(tr("Symlinks (-l)"), m_optionsBox);
    m_linksCheck->setToolTip(tr("-l, --links: copy symlinks as symlinks (without it they are skipped)"));
    m_permsCheck = new QCheckBox(tr("Permissions (-p)"), m_optionsBox);
    m_permsCheck->setToolTip(tr("-p, --perms: preserve permissions"));
    m_timesCheck = new QCheckBox(tr("Times (-t)"), m_optionsBox);
    m_timesCheck->setToolTip(tr("-t, --times: preserve modification times. Without it every run copies all files "
                                "again, because the times never match. On FAT/exFAT add --modify-window=1."));
    m_groupCheck = new QCheckBox(tr("Group (-g)"), m_optionsBox);
    m_groupCheck->setToolTip(tr("-g, --group: preserve the group"));
    m_ownerCheck = new QCheckBox(tr("Owner (-o)"), m_optionsBox);
    m_ownerCheck->setToolTip(tr("-o, --owner: preserve the owner (requires root on the receiving side)"));
    m_devicesCheck = new QCheckBox(tr("Devices, specials (-D)"), m_optionsBox);
    m_devicesCheck->setToolTip(tr("-D: preserve device files and special files (FIFOs, sockets)"));
    m_hardLinksCheck = new QCheckBox(tr("Hard links (-H)"), m_optionsBox);
    m_hardLinksCheck->setToolTip(tr("-H, --hard-links: preserve hard links"));
    m_aclsCheck = new QCheckBox(tr("ACLs (-A)"), m_optionsBox);
    m_aclsCheck->setToolTip(tr("-A, --acls: preserve ACLs (implies -p)"));
    m_xattrsCheck = new QCheckBox(tr("Extended attributes (-X)"), m_optionsBox);
    m_xattrsCheck->setToolTip(tr("-X, --xattrs: preserve extended attributes"));
    preserveGrid->addWidget(m_archiveCheck, 0, 0, 1, 4);
    const QList<QCheckBox *> implied = impliedByArchive();
    for (qsizetype i = 0; i < implied.size(); ++i)
        preserveGrid->addWidget(implied.at(i), 1 + int(i / 4), int(i % 4));
    preserveGrid->addWidget(m_hardLinksCheck, 3, 0);
    preserveGrid->addWidget(m_aclsCheck, 3, 1);
    preserveGrid->addWidget(m_xattrsCheck, 3, 2);
    form->addRow(tr("Copy:"), preserveGrid);

    m_existingCombo = new QComboBox(m_optionsBox);
    m_existingCombo->addItem(tr("Replace files that differ"), int(RsyncOptions::Existing::Replace));
    m_existingCombo->addItem(tr("Skip files that are newer in the destination (-u)"), int(RsyncOptions::Existing::SkipNewer));
    m_existingCombo->addItem(tr("Never replace existing files (--ignore-existing)"),
                             int(RsyncOptions::Existing::IgnoreExisting));
    form->addRow(tr("Existing files:"), m_existingCombo);

    m_checksumCheck = new QCheckBox(tr("Compare files by checksum, not by size and modification time (-c)"), m_optionsBox);
    m_checksumCheck->setToolTip(tr("-c, --checksum: read every file on both sides to decide what to transfer. "
                                   "Slower, but detects files that differ despite the same size and time."));
    form->addRow(QString(), m_checksumCheck);

    auto *outputGrid = new QGridLayout;
    m_progressCombo = new QComboBox(m_optionsBox);
    m_progressCombo->addItem(tr("Per file (--progress)"), int(RsyncOptions::Progress::PerFile));
    m_progressCombo->addItem(tr("Overall (--info=progress2)"), int(RsyncOptions::Progress::Overall));
    m_progressCombo->addItem(tr("None"), int(RsyncOptions::Progress::None));
    m_progressCombo->setToolTip(tr("Per file: progress of each file (with --partial this is -P). "
                                   "Overall: one progress for the whole transfer, better for many small files."));
    m_partialCheck = new QCheckBox(tr("Keep partial files (--partial)"), m_optionsBox);
    m_partialCheck->setToolTip(tr("--partial: keep partially transferred files, so that an interrupted transfer "
                                  "can be resumed faster"));
    m_verboseCheck = new QCheckBox(tr("List files (-v)"), m_optionsBox);
    m_humanCheck = new QCheckBox(tr("Human-readable numbers (-h)"), m_optionsBox);
    outputGrid->addWidget(m_progressCombo, 0, 0);
    outputGrid->addWidget(m_partialCheck, 0, 1);
    outputGrid->addWidget(m_verboseCheck, 1, 0);
    outputGrid->addWidget(m_humanCheck, 1, 1);
    form->addRow(tr("Progress:"), outputGrid);

    auto *transferRow = new QHBoxLayout;
    m_compressCheck = new QCheckBox(tr("Compress (-z)"), m_optionsBox);
    m_compressCheck->setToolTip(tr("-z, --compress: compress data during the transfer (useful for slow networks only)"));
    m_bwLimitCheck = new QCheckBox(tr("Bandwidth limit:"), m_optionsBox);
    m_bwLimitEdit = new QLineEdit(m_optionsBox);
    m_bwLimitEdit->setPlaceholderText(tr("e.g. 500 (KiB/s), 10M"));
    m_bwLimitEdit->setToolTip(tr("--bwlimit=RATE: limit the I/O bandwidth, KiB per second by default"));
    transferRow->addWidget(m_compressCheck);
    transferRow->addSpacing(16);
    transferRow->addWidget(m_bwLimitCheck);
    transferRow->addWidget(m_bwLimitEdit, 1);
    form->addRow(tr("Transfer:"), transferRow);

    auto *shellRow = new QHBoxLayout;
    m_remoteShellCheck = new QCheckBox(tr("Custom (-e)"), m_optionsBox);
    m_remoteShellEdit = new QLineEdit(m_optionsBox);
    m_remoteShellEdit->setPlaceholderText(QStringLiteral("ssh -i ~/.ssh/id_backup"));
    m_remoteShellEdit->setToolTip(tr("-e, --rsh=COMMAND: the remote shell used for user@host:path locations. "
                                     "The port of sftp:// URLs is added automatically (-p PORT)."));
    shellRow->addWidget(m_remoteShellCheck);
    shellRow->addWidget(m_remoteShellEdit, 1);
    form->addRow(tr("Remote shell:"), shellRow);

    m_contentsCheck = new QCheckBox(tr("Copy the contents of source directories, not the directories themselves"),
                                    m_optionsBox);
    m_contentsCheck->setToolTip(tr("Adds a trailing \"/\" to local source directories: \"rsync -a dir/ dest/\" "
                                   "copies the files of dir directly into dest."));
    form->addRow(tr("Directories:"), m_contentsCheck);

    m_deleteCheck = new QCheckBox(tr("Delete files in the destination that are not in the source (--delete)"), m_optionsBox);
    m_deleteCheck->setToolTip(tr("--delete: makes the destination an exact mirror of the source directories. "
                                 "Files that exist only in the destination are deleted!"));
    m_removeSourceCheck = new QCheckBox(tr("Remove source files after copying - move (--remove-source-files)"), m_optionsBox);
    m_removeSourceCheck->setToolTip(tr("--remove-source-files: removes the transferred files from the source "
                                       "(rsync leaves the directories in place)"));
    m_removeSourceDirsCheck = new QCheckBox(tr("Also remove source directories that are left empty"), m_optionsBox);
    m_removeSourceDirsCheck->setToolTip(tr("After a successful move, removes the emptied directories of local sources "
                                           "(only empty ones - a directory with excluded or failed files stays). "
                                           "With \"Copy the contents\" the source directory itself stays."));
    m_dryRunCheck = new QCheckBox(tr("Dry run - only show what would be done (-n)"), m_optionsBox);
    form->addRow(tr("Advanced:"), m_deleteCheck);
    form->addRow(QString(), m_removeSourceCheck);
    auto *removeDirsRow = new QHBoxLayout;
    removeDirsRow->addSpacing(24);
    removeDirsRow->addWidget(m_removeSourceDirsCheck, 1);
    form->addRow(QString(), removeDirsRow);
    form->addRow(QString(), m_dryRunCheck);

    m_verifyCheck = new QCheckBox(tr("Verify after copying (compare checksums of the source and the destination)"),
                                  m_optionsBox);
    m_verifyCheck->setToolTip(tr("After a successful transfer, runs rsync again as a dry run with --checksum "
                                 "--itemize-changes. Every file is read on both sides; any file that differs "
                                 "or is missing is reported.\nWhen moving, the sources are removed only after "
                                 "the verification has passed."));
    form->addRow(tr("Verify:"), m_verifyCheck);

    m_extraEdit = new QLineEdit(m_optionsBox);
    m_extraEdit->setPlaceholderText(tr("e.g. --exclude='*.tmp' --exclude=.cache --mkpath"));
    m_extraEdit->setToolTip(tr("Any other rsync options, written like in a shell (quotes are supported). "
                               "They are also passed to the verification pass (e.g. --exclude)."));
    form->addRow(tr("Additional arguments:"), m_extraEdit);

    auto *settingsRow = new QHBoxLayout;
    auto *defaultsButton = new QPushButton(tr("Restore defaults"), m_optionsBox);
    auto *importButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open")), tr("Import..."), m_optionsBox);
    importButton->setToolTip(tr("Load options from a JSON settings file"));
    auto *exportButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("Export..."), m_optionsBox);
    exportButton->setToolTip(tr("Save the current options to a JSON settings file"));
    settingsRow->addWidget(defaultsButton);
    settingsRow->addStretch(1);
    settingsRow->addWidget(importButton);
    settingsRow->addWidget(exportButton);
    form->addRow(QString(), settingsRow);
    // There are many options - on short screens they scroll instead of squeezing the rows together.
    auto *optionsScroll = new QScrollArea(central);
    optionsScroll->setWidget(m_optionsBox);
    optionsScroll->setWidgetResizable(true);
    optionsScroll->setFrameShape(QFrame::NoFrame);
    optionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tabs = new QTabWidget(central);
    m_tabs->addTab(optionsScroll, QIcon::fromTheme(QStringLiteral("configure")), tr("Options"));
    mainLayout->addWidget(m_tabs, 3);

    // --- Command preview ---
    auto *previewRow = new QHBoxLayout;
    m_previewEdit = new QLineEdit(central);
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    previewRow->addWidget(new QLabel(tr("Command:"), central));
    previewRow->addWidget(m_previewEdit, 1);
    mainLayout->addLayout(previewRow);

    // --- Run ---
    auto *runRow = new QHBoxLayout;
    m_progress = new QProgressBar(central);
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    m_progress->setFormat(QStringLiteral("%p%"));
    m_startButton = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-sync")), tr("Copy"), central);
    m_startButton->setStyleSheet(QStringLiteral("QPushButton:enabled { font-weight: bold; padding: 4px 16px; }"));
    m_startButton->setDefault(true);
    m_cancelButton = new QPushButton(QIcon::fromTheme(QStringLiteral("process-stop")), tr("Cancel"), central);
    m_closeWhenFinishedCheck = new QCheckBox(tr("Close when finished"), central);
    m_closeWhenFinishedCheck->setToolTip(tr("Quit Rsync GUI after a successful transfer. After errors, verification "
                                            "differences, cancelling or a dry run the window stays open."));
    runRow->addWidget(m_progress, 1);
    runRow->addWidget(m_closeWhenFinishedCheck);
    runRow->addWidget(m_startButton);
    runRow->addWidget(m_cancelButton);
    mainLayout->addLayout(runRow);

    m_currentFile = new QLabel(central);
    m_currentFile->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_currentFile->setTextFormat(Qt::PlainText);
    m_transferStatus = new QLabel(central);
    m_transferStatus->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mainLayout->addWidget(m_currentFile);
    mainLayout->addWidget(m_transferStatus);

    // --- Log ---
    auto *logPage = new QWidget(m_tabs);
    auto *logLayout = new QVBoxLayout(logPage);
    m_log = new QPlainTextEdit(logPage);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(20000);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMinimumHeight(120);
    auto *clearLog = new QPushButton(tr("Clear log"), logPage);
    logLayout->addWidget(m_log, 1);
    logLayout->addWidget(clearLog, 0, Qt::AlignRight);
    m_tabs->addTab(logPage, QIcon::fromTheme(QStringLiteral("view-list-text")), tr("Log"));

    setCentralWidget(central);
    resize(900, 900);

    connect(m_sourceEdit, &QLineEdit::returnPressed, this, &MainWindow::addTypedSource);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addTypedSource);
    connect(addFilesButton, &QPushButton::clicked, this, &MainWindow::browseFiles);
    connect(addDirButton, &QPushButton::clicked, this, &MainWindow::browseDirectory);
    connect(pasteButton, &QPushButton::clicked, this, &MainWindow::pasteSources);
    connect(pasteAction, &QAction::triggered, this, &MainWindow::pasteSources);
    connect(m_removeSourcesButton, &QPushButton::clicked, this, &MainWindow::removeSelectedSources);
    connect(removeAction, &QAction::triggered, this, &MainWindow::removeSelectedSources);
    connect(m_clearSourcesButton, &QPushButton::clicked, this, [this] {
        m_sourceList->clear();
        updateSourceInfo();
        updatePreview();
        updateControls();
    });
    connect(m_sourceList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateControls);
    connect(destBrowse, &QPushButton::clicked, this, &MainWindow::browseDestination);
    connect(m_destinationCombo, &QComboBox::currentTextChanged, this, [this] {
        updateDestinationInfo();
        updatePreview();
        updateControls();
    });

    connect(programBrowse, &QPushButton::clicked, this, &MainWindow::browseProgram);
    connect(defaultsButton, &QPushButton::clicked, this, [this] {
        RsyncOptions defaults;
        defaults.program = m_programCombo->count() > 0 ? m_programCombo->itemText(0) : defaults.program;
        applyOptions(defaults);
    });
    connect(importButton, &QPushButton::clicked, this, &MainWindow::importSettings);
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportSettings);
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startCopy);
    connect(m_cancelButton, &QPushButton::clicked, this, &MainWindow::cancelCopy);
    connect(clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    // A window preference, not an rsync option: remembered right away, also in a preset (session-only) window.
    connect(m_closeWhenFinishedCheck, &QCheckBox::toggled, this, [](bool on) {
        QSettings().setValue(QStringLiteral("window/closeWhenFinished"), on);
    });

    const auto refresh = [this] {
        updatePreview();
        updateControls();
    };
    connect(m_programCombo, &QComboBox::currentTextChanged, this, refresh);
    connect(m_archiveCheck, &QCheckBox::toggled, this, &MainWindow::onArchiveToggled);
    for (QCheckBox *box : impliedByArchive())
        connect(box, &QCheckBox::toggled, this, refresh);
    for (QCheckBox *box : {m_archiveCheck, m_hardLinksCheck, m_aclsCheck, m_xattrsCheck,
                           m_checksumCheck, m_partialCheck, m_verboseCheck, m_humanCheck, m_compressCheck,
                           m_bwLimitCheck, m_remoteShellCheck, m_contentsCheck, m_deleteCheck, m_removeSourceCheck, m_removeSourceDirsCheck,
                           m_dryRunCheck, m_verifyCheck}) {
        connect(box, &QCheckBox::toggled, this, refresh);
    }
    for (QComboBox *combo : {m_existingCombo, m_progressCombo})
        connect(combo, &QComboBox::currentIndexChanged, this, refresh);
    for (QLineEdit *edit : {m_bwLimitEdit, m_remoteShellEdit, m_extraEdit})
        connect(edit, &QLineEdit::textChanged, this, refresh);
}

void MainWindow::buildMenus()
{
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    m_presetMenu = fileMenu->addMenu(QIcon::fromTheme(QStringLiteral("bookmarks")), tr("Load &preset"));
    connect(m_presetMenu, &QMenu::aboutToShow, this, &MainWindow::populatePresetMenu);
    m_importAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-open")),
                                         tr("&Import settings..."), this, &MainWindow::importSettings);
    m_importAction->setShortcut(QKeySequence::Open);
    m_exportAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("document-save")),
                                         tr("&Export settings..."), this, &MainWindow::exportSettings);
    m_exportAction->setShortcut(QKeySequence::Save);
    m_recentSettingsMenu = fileMenu->addMenu(QIcon::fromTheme(QStringLiteral("document-open-recent")),
                                             tr("&Recent settings"));
    connect(m_recentSettingsMenu, &QMenu::aboutToShow, this, &MainWindow::populateRecentSettingsMenu);
    fileMenu->addSeparator();
    QAction *quit = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("application-exit")), tr("&Quit"), this,
                                        &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, &MainWindow::showAbout);
}

void MainWindow::populatePresetMenu()
{
    m_presetMenu->clear();
    // The same names as in the Dolphin menu; the file and the command are in the tooltip.
    const QList<RsyncPreset> presets = RsyncOptions::presets();
    if (presets.isEmpty()) {
        m_presetMenu->addAction(tr("No presets found"))->setEnabled(false);
        return;
    }
    m_presetMenu->setToolTipsVisible(true);
    for (const RsyncPreset &preset : presets) {
        const QString file = preset.file;
        QAction *action = m_presetMenu->addAction(preset.label, this, [this, file] {
            importSettingsFile(file, Persistence::SessionOnly);
        });
        if (preset.error.isEmpty()) {
            QString command = commandLine(QStringLiteral("rsync"), preset.options.arguments());
            if (preset.options.verifiedMove())
                command += tr(" + verification pass, then --remove-source-files");
            else if (preset.options.canVerify())
                command += tr(" + verification pass");
            action->setToolTip(tr("%1\n%2").arg(command, file));
        } else {
            action->setEnabled(false);
            action->setToolTip(preset.error);
        }
    }
    m_presetMenu->addSeparator();
    m_presetMenu->addAction(tr("Presets are used for this session only"))->setEnabled(false);
}

void MainWindow::populateRecentSettingsMenu()
{
    m_recentSettingsMenu->clear();
    m_recentSettingsMenu->setToolTipsVisible(true);
    // Files that no longer exist are dropped from the list.
    QStringList files = QSettings().value(kRecentSettingsKey).toStringList();
    files.erase(std::remove_if(files.begin(), files.end(), [](const QString &f) { return !QFileInfo(f).isFile(); }),
                files.end());
    QSettings().setValue(kRecentSettingsKey, files);
    if (files.isEmpty()) {
        m_recentSettingsMenu->addAction(tr("No recent settings files"))->setEnabled(false);
        return;
    }
    for (const QString &file : std::as_const(files)) {
        QString error;
        QString name;
        RsyncOptions::loadFromFile(file, &error, RsyncOptions(), &name);
        const QString fileName = QFileInfo(file).fileName();
        QString text = name.isEmpty() || name == QFileInfo(file).completeBaseName()
            ? fileName
            : tr("%1 (%2)").arg(name, fileName);
        QAction *action = m_recentSettingsMenu->addAction(text.replace(QLatin1Char('&'), QStringLiteral("&&")), this,
                                                          [this, file] { importSettingsFile(file, Persistence::Remember); });
        action->setToolTip(error.isEmpty() ? file : error);
        action->setEnabled(error.isEmpty());
    }
    m_recentSettingsMenu->addSeparator();
    m_recentSettingsMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-clear-history")), tr("Clear list"), this,
                                    [] { QSettings().remove(kRecentSettingsKey); });
}

void MainWindow::rememberRecentSettingsFile(const QString &file)
{
    const QString path = QFileInfo(file).absoluteFilePath();
    // Presets have their own menu.
    for (const QString &dir : RsyncOptions::presetSearchPaths()) {
        if (QFileInfo(path).absolutePath() == QFileInfo(dir).absoluteFilePath())
            return;
    }
    QStringList files = QSettings().value(kRecentSettingsKey).toStringList();
    files.removeAll(path);
    files.prepend(path);
    QSettings().setValue(kRecentSettingsKey, files.mid(0, kMaxRecentSettings));
}

void MainWindow::loadSettings()
{
    QSettings settings;
    RsyncOptions options = RsyncOptions::load(settings);
    if (!settings.contains(QStringLiteral("options/program")) && m_programCombo->count() > 0)
        options.program = m_programCombo->itemText(0);
    applyOptions(options);
    const QStringList recent = settings.value(QStringLiteral("destinations/recent")).toStringList();
    m_destinationCombo->addItems(recent);
    m_destinationCombo->setCurrentIndex(-1);
    m_destinationCombo->setEditText(QString());
    m_closeWhenFinishedCheck->setChecked(settings.value(QStringLiteral("window/closeWhenFinished"), false).toBool());
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
}

void MainWindow::saveSettings() const
{
    QSettings settings;
    if (m_rememberOptions)
        currentOptions().save(settings);
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
}

void MainWindow::rememberDestination(const QString &destination)
{
    if (destination.isEmpty())
        return;
    QStringList recent{destination};
    for (int i = 0; i < m_destinationCombo->count() && recent.size() < kMaxRecentDestinations; ++i) {
        if (m_destinationCombo->itemText(i) != destination)
            recent << m_destinationCombo->itemText(i);
    }
    const QSignalBlocker blocker(m_destinationCombo);
    m_destinationCombo->clear();
    m_destinationCombo->addItems(recent);
    m_destinationCombo->setCurrentIndex(0);
    QSettings().setValue(QStringLiteral("destinations/recent"), recent);
}

void MainWindow::setOptionsSource(const QString &description)
{
    m_optionsBox->setTitle(description.isEmpty() ? optionsBoxTitle()
                                                 : tr("%1 - %2").arg(optionsBoxTitle(), description));
}

RsyncOptions MainWindow::currentOptions() const
{
    RsyncOptions o;
    o.program = m_programCombo->currentText().trimmed();
    o.archive = m_archiveCheck->isChecked();
    // While -a is on, the implied boxes only show what it includes; the user's own choice is kept aside.
    const QList<QCheckBox *> implied = impliedByArchive();
    bool *parts[] = {&o.recursive, &o.links, &o.perms, &o.times, &o.group, &o.owner, &o.devices};
    for (qsizetype i = 0; i < implied.size(); ++i)
        *parts[i] = o.archive ? m_impliedValues.at(i) : implied.at(i)->isChecked();
    o.hardLinks = m_hardLinksCheck->isChecked();
    o.acls = m_aclsCheck->isChecked();
    o.xattrs = m_xattrsCheck->isChecked();
    o.existing = static_cast<RsyncOptions::Existing>(m_existingCombo->currentData().toInt());
    o.checksum = m_checksumCheck->isChecked();
    o.progress = static_cast<RsyncOptions::Progress>(m_progressCombo->currentData().toInt());
    o.partial = m_partialCheck->isChecked();
    o.verbose = m_verboseCheck->isChecked();
    o.human = m_humanCheck->isChecked();
    o.compress = m_compressCheck->isChecked();
    o.useBwLimit = m_bwLimitCheck->isChecked();
    o.bwLimit = m_bwLimitEdit->text().trimmed();
    o.useRemoteShell = m_remoteShellCheck->isChecked();
    o.remoteShell = m_remoteShellEdit->text().trimmed();
    o.contentsOnly = m_contentsCheck->isChecked();
    o.deleteExtraneous = m_deleteCheck->isChecked();
    o.removeSourceFiles = m_removeSourceCheck->isChecked();
    o.removeSourceDirs = m_removeSourceDirsCheck->isChecked();
    o.dryRun = m_dryRunCheck->isChecked();
    o.verifyAfter = m_verifyCheck->isChecked();
    o.extraArguments = m_extraEdit->text().trimmed();
    return o;
}

void MainWindow::applyOptions(const RsyncOptions &o)
{
    if (m_programCombo->findText(o.program) < 0 && !o.program.isEmpty())
        m_programCombo->addItem(o.program);
    m_programCombo->setCurrentText(o.program);
    // First show the parts of -a as they are in `o`, then let onArchiveToggled() set them aside if -a is on.
    m_archiveCheck->setChecked(false);
    const QList<QCheckBox *> implied = impliedByArchive();
    const bool parts[] = {o.recursive, o.links, o.perms, o.times, o.group, o.owner, o.devices};
    for (qsizetype i = 0; i < implied.size(); ++i)
        implied.at(i)->setChecked(parts[i]);
    m_archiveCheck->setChecked(o.archive);
    m_hardLinksCheck->setChecked(o.hardLinks);
    m_aclsCheck->setChecked(o.acls);
    m_xattrsCheck->setChecked(o.xattrs);
    m_existingCombo->setCurrentIndex(qMax(0, m_existingCombo->findData(int(o.existing))));
    m_checksumCheck->setChecked(o.checksum);
    m_progressCombo->setCurrentIndex(qMax(0, m_progressCombo->findData(int(o.progress))));
    m_partialCheck->setChecked(o.partial);
    m_verboseCheck->setChecked(o.verbose);
    m_humanCheck->setChecked(o.human);
    m_compressCheck->setChecked(o.compress);
    m_bwLimitCheck->setChecked(o.useBwLimit);
    m_bwLimitEdit->setText(o.bwLimit);
    m_remoteShellCheck->setChecked(o.useRemoteShell);
    m_remoteShellEdit->setText(o.remoteShell);
    m_contentsCheck->setChecked(o.contentsOnly);
    m_deleteCheck->setChecked(o.deleteExtraneous);
    m_removeSourceCheck->setChecked(o.removeSourceFiles);
    m_removeSourceDirsCheck->setChecked(o.removeSourceDirs);
    m_dryRunCheck->setChecked(o.dryRun);
    m_verifyCheck->setChecked(o.verifyAfter);
    m_extraEdit->setText(o.extraArguments);
    updatePreview();
    updateControls();
}

QList<QCheckBox *> MainWindow::impliedByArchive() const
{
    // The order of -rlptgoD, also used by currentOptions() and applyOptions().
    return {m_recursiveCheck, m_linksCheck, m_permsCheck, m_timesCheck, m_groupCheck, m_ownerCheck, m_devicesCheck};
}

void MainWindow::onArchiveToggled(bool on)
{
    const QList<QCheckBox *> implied = impliedByArchive();
    for (qsizetype i = 0; i < implied.size(); ++i) {
        const QSignalBlocker blocker(implied.at(i));
        if (on) {
            m_impliedValues[i] = implied.at(i)->isChecked();
            implied.at(i)->setChecked(true);
        } else {
            implied.at(i)->setChecked(m_impliedValues.at(i));
        }
    }
}

void MainWindow::addSources(const QStringList &paths)
{
    QStringList existing;
    for (const Location &loc : sources())
        existing << loc.toString();
    int added = 0;
    for (const QString &raw : paths) {
        const Location loc = Location::parse(raw);
        if (!loc.isValid() || existing.contains(loc.toString()))
            continue;
        auto *item = new QListWidgetItem(LocalPath::display(loc.toString()), m_sourceList);
        item->setData(kLocationRole, loc.toString());
        refreshSourceItem(item);
        existing << loc.toString();
        ++added;
    }
    if (added > 0)
        m_sourceList->scrollToBottom();
    updateSourceInfo();
    updatePreview();
    updateControls();
}

QList<Location> MainWindow::sources() const
{
    QList<Location> list;
    for (int i = 0; i < m_sourceList->count(); ++i)
        list << Location::parse(m_sourceList->item(i)->data(kLocationRole).toString());
    return list;
}

Location MainWindow::destination() const
{
    return Location::parse(m_destinationCombo->currentText());
}

void MainWindow::setDestination(const QString &destination)
{
    const Location loc = Location::parse(destination);
    m_destinationCombo->setEditText(loc.isValid() ? loc.toString() : destination);
}

void MainWindow::refreshSourceItem(QListWidgetItem *item)
{
    const Location loc = Location::parse(item->data(kLocationRole).toString());
    QString icon;
    QString tooltip;
    if (loc.isRemote()) {
        icon = QStringLiteral("folder-network");
        tooltip = tr("remote (%1 over ssh)").arg(loc.port > 0 ? tr("%1, port %2").arg(loc.host).arg(loc.port) : loc.host);
    } else {
        const LocalPath::Info info = LocalPath::info(loc.path);
        if (info.isSymLink) {
            icon = QStringLiteral("emblem-symbolic-link");
            tooltip = tr("symbolic link to %1 (copied as a link in archive mode)").arg(LocalPath::display(info.symLinkTarget));
        } else if (info.isDir) {
            icon = QStringLiteral("folder");
            tooltip = tr("directory");
        } else if (info.isFile) {
            icon = QStringLiteral("text-x-generic");
            tooltip = tr("file, %1").arg(QLocale().formattedDataSize(info.size));
        } else if (info.exists) {
            icon = QStringLiteral("drive-harddisk");
            tooltip = tr("special file");
        } else {
            icon = QStringLiteral("dialog-error");
            tooltip = tr("does not exist");
        }
    }
    if (!LocalPath::isPlain(loc.path))
        tooltip += tr("\nThe name is not valid UTF-8 - shown with \\x escapes, copied with the exact bytes.");
    item->setIcon(QIcon::fromTheme(icon));
    item->setToolTip(tooltip);
}

void MainWindow::refreshSources()
{
    for (int i = 0; i < m_sourceList->count(); ++i)
        refreshSourceItem(m_sourceList->item(i));
    updateSourceInfo();
    updateDestinationInfo();
    updatePreview();
    updateControls();
}

void MainWindow::updateSourceInfo()
{
    const QList<Location> list = sources();
    if (list.isEmpty()) {
        m_sourceInfo->setText(tr("No sources. Add files or directories, paste paths or drop them here."));
        return;
    }
    if (list.size() == 1) {
        m_sourceInfo->setText(tr("Type: %1").arg(m_sourceList->item(0)->toolTip()));
        return;
    }
    int files = 0;
    int dirs = 0;
    int remote = 0;
    int missing = 0;
    for (const Location &loc : list) {
        if (loc.isRemote()) {
            ++remote;
            continue;
        }
        const LocalPath::Info info = LocalPath::info(loc.path);
        if (!info.exists)
            ++missing;
        else if (info.isDir)
            ++dirs;
        else
            ++files;
    }
    QString text = tr("%1 sources: %2 file(s), %3 director(y/ies)").arg(list.size()).arg(files).arg(dirs);
    if (remote > 0)
        text += tr(", %1 remote").arg(remote);
    if (missing > 0)
        text += tr(", %1 missing (they will be skipped)").arg(missing);
    m_sourceInfo->setText(text);
}

void MainWindow::updateDestinationInfo()
{
    const Location dest = destination();
    QString text;
    if (!dest.isValid()) {
        text = tr("Choose the directory to copy into.");
    } else if (dest.isRemote()) {
        text = tr("Remote destination on %1 (over ssh). rsync creates the last directory of the path if it is missing.")
                   .arg(dest.host);
    } else {
        if (LocalPath::isDirectory(dest.path)) {
            const QStorageInfo storage(LocalPath::isPlain(dest.path) ? dest.path : QString());
            text = storage.isValid() && LocalPath::isPlain(dest.path)
                ? tr("Existing directory, %1 free.").arg(QLocale().formattedDataSize(storage.bytesAvailable()))
                : tr("Existing directory.");
            if (RsyncJob::isFatFileSystem(dest.path))
                text += tr(" FAT file system: file times are compared with a tolerance of 1 s (--modify-window=1).");
        } else if (LocalPath::info(dest.path).exists) {
            text = tr("<b>Not a directory:</b> %1").arg(LocalPath::display(dest.path).toHtmlEscaped());
        } else {
            text = tr("The directory %1 does not exist - it will be created.").arg(LocalPath::display(dest.path));
        }
        if (!LocalPath::isPlain(dest.path))
            text += tr(" (%1)").arg(LocalPath::display(dest.path));
    }
    m_destinationInfo->setText(text);
}

void MainWindow::updatePreview()
{
    const RsyncOptions options = currentOptions();
    const Location dest = destination();
    const RsyncJob::Plan plan = RsyncJob::plan(sources(), dest.isValid() ? dest : Location::parse(QStringLiteral("/DEST")),
                                               options);
    QStringList args = options.arguments(plan.transfers.isEmpty() ? 0 : plan.transfers.first().port);
    args << QStringLiteral("--");
    QString text;
    if (plan.transfers.isEmpty()) {
        text = commandLine(options.program, args) + tr(" <sources> ")
            + (dest.isValid() ? shellQuote(dest.rsyncSpec() + QLatin1Char('/')) : tr("<destination>/"));
    } else {
        const RsyncJob::Transfer &first = plan.transfers.first();
        args << first.sources;
        text = commandLine(options.program, args) + QLatin1Char(' ')
            + (dest.isValid() ? shellQuote(first.destination) : tr("<destination>/"));
        if (plan.transfers.size() > 1)
            text += tr("   (+ %1 more rsync run(s) for other hosts)").arg(plan.transfers.size() - 1);
    }
    if (options.verifiedMove())
        text += tr("   (+ verification pass, then the sources are removed)");
    else if (options.canVerify())
        text += tr("   (+ verification pass)");
    m_previewEdit->setText(text);
    m_previewEdit->setCursorPosition(0);
}

void MainWindow::updateControls()
{
    const bool running = m_job && m_job->isRunning();
    m_sourceBox->setEnabled(!running);
    m_destinationBox->setEnabled(!running);
    m_optionsBox->setEnabled(!running);
    if (m_importAction)
        m_importAction->setEnabled(!running);
    if (m_presetMenu)
        m_presetMenu->setEnabled(!running);
    if (m_recentSettingsMenu)
        m_recentSettingsMenu->setEnabled(!running);
    for (QCheckBox *box : impliedByArchive())
        box->setEnabled(!m_archiveCheck->isChecked());
    m_bwLimitEdit->setEnabled(m_bwLimitCheck->isChecked());
    m_remoteShellEdit->setEnabled(m_remoteShellCheck->isChecked());
    m_removeSourceDirsCheck->setEnabled(m_removeSourceCheck->isChecked());
    m_removeSourcesButton->setEnabled(!m_sourceList->selectedItems().isEmpty());
    m_clearSourcesButton->setEnabled(m_sourceList->count() > 0);
    m_startButton->setEnabled(!running && m_sourceList->count() > 0 && destination().isValid());
    m_startButton->setText(m_dryRunCheck->isChecked() ? tr("Dry run")
                                                      : m_removeSourceCheck->isChecked() ? tr("Move") : tr("Copy"));
    m_cancelButton->setEnabled(running);

    QString error = currentOptions().validate();
    if (error.isEmpty() && m_verifyCheck->isChecked() && !currentOptions().canVerify())
        error = tr("Verification is not done for a dry run.");
    statusBar()->showMessage(error);
}

QStringList MainWindow::pathsFromMimeData(const QMimeData *mime)
{
    QStringList paths;
    if (!mime)
        return paths;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            // Encoded URLs keep the exact bytes of names that are not valid UTF-8 (see LocalPath).
            if (url.isLocalFile() || url.scheme() == QStringLiteral("sftp"))
                paths << url.toString(QUrl::FullyEncoded);
        }
    } else {
        paths = mime->text().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    return paths;
}

void MainWindow::addTypedSource()
{
    if (m_sourceEdit->text().trimmed().isEmpty())
        return;
    addSources({m_sourceEdit->text()});
    m_sourceEdit->clear();
}

void MainWindow::pasteSources()
{
    addSources(pathsFromMimeData(QApplication::clipboard()->mimeData()));
}

void MainWindow::removeSelectedSources()
{
    qDeleteAll(m_sourceList->selectedItems());
    updateSourceInfo();
    updatePreview();
    updateControls();
}

void MainWindow::browseFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select files to copy"), QDir::homePath(),
                                                            QString(), nullptr, QFileDialog::DontResolveSymlinks);
    addSources(files);
}

void MainWindow::browseDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select a directory to copy"), QDir::homePath(),
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        addSources({dir});
}

void MainWindow::browseDestination()
{
    // Start in the current destination, or next to the first local source (like a file manager would).
    QString start;
    const Location dest = destination();
    if (dest.isValid() && !dest.isRemote() && LocalPath::isDirectory(dest.path))
        start = dest.path;
    if (start.isEmpty() && m_destinationCombo->count() > 0) {
        const Location recent = Location::parse(m_destinationCombo->itemText(0));
        if (!recent.isRemote() && LocalPath::isDirectory(recent.path))
            start = recent.path;
    }
    if (start.isEmpty()) {
        for (const Location &loc : sources()) {
            if (!loc.isRemote()) {
                start = QFileInfo(loc.path).absolutePath();
                break;
            }
        }
    }
    if (start.isEmpty())
        start = QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select the destination directory"), start,
                                                          QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty()) {
        setDestination(dir);
        m_startButton->setFocus();
    }
}

void MainWindow::browseProgram()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Select the rsync program"), QStringLiteral("/usr/bin"));
    if (!file.isEmpty())
        m_programCombo->setCurrentText(file);
}

QString MainWindow::settingsDialogDir() const
{
    const QString dir = QSettings().value(QStringLiteral("paths/settingsDir")).toString();
    return !dir.isEmpty() && QFileInfo(dir).isDir() ? dir : QDir::homePath();
}

void MainWindow::rememberSettingsDialogDir(const QString &file) const
{
    QSettings().setValue(QStringLiteral("paths/settingsDir"), QFileInfo(file).absolutePath());
}

bool MainWindow::importSettingsFile(const QString &path, Persistence persistence)
{
    QString error;
    QString name;
    // Keys missing from the file keep the current values (e.g. presets do not choose the program).
    const RsyncOptions options = RsyncOptions::loadFromFile(path, &error, currentOptions(), &name);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Import failed"), error);
        return false;
    }
    if (name.isEmpty())
        name = QFileInfo(path).completeBaseName();
    applyOptions(options);
    rememberRecentSettingsFile(path);

    if (persistence == Persistence::Remember) {
        m_rememberOptions = true;
        setOptionsSource(QString());
        saveSettings();
        appendLog(RsyncJob::LogLevel::Info, tr("Imported settings from %1").arg(path));
    } else {
        // Session only: the remembered options stay untouched until settings are imported explicitly.
        m_rememberOptions = false;
        setOptionsSource(tr("%1 - session only, not remembered").arg(options.label(name)));
        appendLog(RsyncJob::LogLevel::Info, tr("Loaded \"%1\" from %2 (session only)").arg(name, path));
    }
    // The validation message (e.g. a program missing on this machine) takes precedence in the status bar.
    if (options.validate().isEmpty())
        statusBar()->showMessage(tr("Loaded settings \"%1\"").arg(name), 8000);
    return true;
}

bool MainWindow::loadPreset(const QString &name)
{
    const QString file = RsyncOptions::locatePreset(name);
    if (file.isEmpty()) {
        QMessageBox::warning(this, tr("Preset not found"),
                             tr("Preset \"%1\" was not found. Searched in:\n%2")
                                 .arg(name, RsyncOptions::presetSearchPaths().join(QLatin1Char('\n'))));
        return false;
    }
    return importSettingsFile(file, Persistence::SessionOnly);
}

void MainWindow::importSettings()
{
    if (m_job && m_job->isRunning())
        return;
    const QString file = QFileDialog::getOpenFileName(this, tr("Import settings"), settingsDialogDir(),
                                                      settingsFileFilter());
    if (file.isEmpty())
        return;
    rememberSettingsDialogDir(file);
    importSettingsFile(file, Persistence::Remember);
}

void MainWindow::exportSettings()
{
    const QString start = QDir(settingsDialogDir()).filePath(QStringLiteral("rsync-gui-settings.json"));
    QString file = QFileDialog::getSaveFileName(this, tr("Export settings"), start, settingsFileFilter());
    if (file.isEmpty())
        return;
    if (QFileInfo(file).suffix().isEmpty())
        file += QStringLiteral(".json");
    rememberSettingsDialogDir(file);

    QString error;
    if (!currentOptions().saveToFile(file, &error)) {
        QMessageBox::warning(this, tr("Export failed"), error);
        return;
    }
    rememberRecentSettingsFile(file);
    appendLog(RsyncJob::LogLevel::Info, tr("Exported settings to %1").arg(file));
    statusBar()->showMessage(tr("Exported settings to %1").arg(file), 8000);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, tr("About Rsync GUI"),
                       tr("<b>Rsync GUI</b> %1<br><br>"
                          "A graphical front-end for copying files with <code>rsync</code>.<br><br>"
                          "<b>Created with AI:</b> the code, tests and documentation were written by "
                          "Claude (Anthropic) in Claude Code, following the author's requirements, and "
                          "checked with automated tests and by hand. Review the code before relying on it, "
                          "especially for moving files.<br><br>"
                          "Licensed under the MIT License.")
                           .arg(QApplication::applicationVersion()));
}

void MainWindow::startCopy()
{
    if (m_job && m_job->isRunning())
        return;

    RsyncOptions options = currentOptions();
    if (const QString error = options.validate(); !error.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid options"), error);
        return;
    }

    const Location dest = destination();
    if (!dest.isValid()) {
        QMessageBox::warning(this, tr("No destination"), tr("Choose the destination directory first."));
        return;
    }
    if (!dest.isRemote()) {
        if (LocalPath::info(dest.path).exists && !LocalPath::isDirectory(dest.path)) {
            QMessageBox::warning(this, tr("Invalid destination"), tr("%1 is not a directory.").arg(LocalPath::display(dest.path)));
            return;
        }
    }

    RsyncJob::Plan plan = RsyncJob::plan(sources(), dest, options);
    if (!plan.error.isEmpty()) {
        QMessageBox::warning(this, tr("Cannot continue"), plan.error);
        return;
    }
    if (!plan.missing.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, tr("Some sources do not exist"),
            tr("These sources do not exist:\n%1\n\nContinue without them?")
                .arg(LocalPath::display(plan.missing.join(QLatin1Char('\n')))));
        if (answer != QMessageBox::Yes)
            return;
    }

    if (!dest.isRemote() && !LocalPath::info(dest.path).exists && !options.dryRun) {
        const auto answer = QMessageBox::question(this, tr("Create destination"),
                                                  tr("The directory %1 does not exist. Create it?").arg(LocalPath::display(dest.path)));
        if (answer != QMessageBox::Yes)
            return;
        if (!LocalPath::makePath(dest.path)) {
            QMessageBox::warning(this, tr("Create destination"), tr("Cannot create the directory %1.").arg(LocalPath::display(dest.path)));
            return;
        }
    }

    // FAT keeps file times to 2 s: without a tolerance rsync would copy half of the files again on every run.
    if (!dest.isRemote() && (options.archive || options.times))
        options.modifyWindow = RsyncJob::modifyWindowFor(dest.path, !options.dryRun);

    // Copying is not destructive - only the options that delete files are confirmed.
    if (!options.dryRun && (options.deleteExtraneous || options.removeSourceFiles)) {
        QStringList commands;
        for (const RsyncJob::Transfer &transfer : std::as_const(plan.transfers)) {
            commands << commandLine(options.program, options.arguments(transfer.port) << QStringLiteral("--")
                                                                                         << transfer.sources
                                                                                         << transfer.destination)
                            .toHtmlEscaped();
        }
        QString text;
        if (options.deleteExtraneous)
            text += tr("<b>--delete</b>: files in the destination that do not exist in the source will be "
                       "<b>deleted</b>.<br>");
        if (options.verifiedMove())
            text += tr("<b>--remove-source-files</b>: the copied files will be <b>removed from the source</b> "
                       "(move) - only after the verification has found no differences.<br>");
        else if (options.removeSourceFiles)
            text += tr("<b>--remove-source-files</b>: the copied files will be <b>removed from the source</b> "
                       "(move).<br>");
        if (options.removesSourceDirs())
            text += tr("Source directories left empty will be removed too.<br>");
        text += tr("<br>Command:<br><code>%1</code><br><br>Consider a dry run (-n) first.")
                    .arg(commands.join(QStringLiteral("<br>")));
        QMessageBox box(QMessageBox::Warning, tr("Confirm"), text, QMessageBox::NoButton, this);
        box.setTextFormat(Qt::RichText);
        QPushButton *confirm = box.addButton(options.removeSourceFiles ? tr("Move") : tr("Copy and delete"),
                                             QMessageBox::DestructiveRole);
        QPushButton *abort = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.setDefaultButton(abort);
        box.exec();
        if (box.clickedButton() != confirm)
            return;
    }

    rememberDestination(dest.toString());
    saveSettings();
    delete m_job;
    m_job = new RsyncJob(std::move(plan.transfers), options, this);
    startDesktopProgress(options, dest);
    connect(m_job, &RsyncJob::log, this, &MainWindow::appendLog);
    connect(m_job, &RsyncJob::progress, m_progress, &QProgressBar::setValue);
    connect(m_job, &RsyncJob::currentFileChanged, this, [this](const QString &file) {
        m_currentFile->setText(file);
    });
    connect(m_job, &RsyncJob::phaseChanged, this, [this](const QString &phase) {
        if (!phase.isEmpty())
            m_currentFile->setText(phase + QStringLiteral("..."));
    });
    connect(m_job, &RsyncJob::statusChanged, m_transferStatus, &QLabel::setText);
    connect(m_job, &RsyncJob::finished, this, &MainWindow::onJobFinished);

    for (const QString &note : std::as_const(plan.notes))
        appendLog(RsyncJob::LogLevel::Warning, note);
    for (const QString &missing : std::as_const(plan.missing))
        appendLog(RsyncJob::LogLevel::Warning, tr("Skipped missing source: %1").arg(LocalPath::display(missing)));
    appendLogLines(RsyncJob::LogLevel::Info,
                   tr("=== Start: %1 source(s) to %2 ===").arg(sources().size()).arg(LocalPath::display(dest.toString())),
                   true);
    if (options.modifyWindow > 0) {
        appendLog(RsyncJob::LogLevel::Info,
                  tr("The destination keeps file times only to 2 seconds (FAT) - added --modify-window=%1, "
                     "so that unchanged files are not copied again.")
                      .arg(options.modifyWindow));
    }
    // Without progress output from rsync the bar can only show that something is happening.
    if (options.progress == RsyncOptions::Progress::None && !options.canVerify())
        m_progress->setRange(0, 0);
    else
        m_progress->setRange(0, 1000);
    m_tabs->setCurrentIndex(m_tabs->indexOf(m_log->parentWidget()));
    m_job->start();
    updateControls();
}

void MainWindow::startDesktopProgress(const RsyncOptions &options, const Location &dest)
{
    const QList<Location> list = sources();
    QString source = list.isEmpty() ? QString() : LocalPath::display(list.first().toString());
    if (list.size() > 1)
        source = tr("%1 (+%2 more)").arg(source).arg(list.size() - 1);
    const QString destUrl = dest.isRemote()
        ? QString()
        : QStringLiteral("file://") + QString::fromLatin1(LocalPath::encode(dest.path).toPercentEncoding("/"));
    const QString title = options.dryRun ? tr("Dry run") : options.removeSourceFiles ? tr("Moving") : tr("Copying");
    m_desktopProgress->start(title, source, LocalPath::display(dest.toString()), destUrl);
    connect(m_job, &RsyncJob::progress, m_desktopProgress, &DesktopProgress::setProgress);
    connect(m_job, &RsyncJob::statusChanged, m_desktopProgress, [this](QString status) {
        // Plasma shows the percentage itself; keep the size, speed and ETA.
        static const QRegularExpression percent(QStringLiteral("^\\d+%\\s*"));
        m_desktopProgress->setInfo(status.remove(percent));
    });
    connect(m_job, &RsyncJob::transferRateChanged, m_desktopProgress, &DesktopProgress::setSpeed);
    measureSourcesForDesktopProgress(options);
    connect(m_job, &RsyncJob::phaseChanged, m_desktopProgress, [this](const QString &phase) {
        if (phase.isEmpty())
            return;
        m_desktopProgress->setTitle(phase);
        m_desktopProgress->setSpeed(0, -1); // verification and removal report no speed
    });
}

void MainWindow::measureSourcesForDesktopProgress(const RsyncOptions &options)
{
    // Plasma's speed graph needs the total size, which rsync does not report: the local sources are
    // measured in the background. With a remote source there is no total (and no graph).
    stopMeasuringSources();
    QStringList paths;
    for (const Location &loc : sources()) {
        if (loc.isRemote())
            return;
        paths << loc.path;
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_measureCancel = cancel;
    const bool recursive = options.archive || options.recursive;
    auto *watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, cancel] {
        watcher->deleteLater();
        if (!cancel->load())
            m_desktopProgress->setTotalBytes(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run([paths, recursive, cancel] {
        return LocalPath::totalSize(paths, recursive, cancel.get());
    }));
}

void MainWindow::stopMeasuringSources()
{
    if (m_measureCancel)
        m_measureCancel->store(true);
    m_measureCancel.reset();
}

void MainWindow::cancelCopy()
{
    if (m_job)
        m_job->cancel();
}

void MainWindow::appendLog(RsyncJob::LogLevel level, const QString &text)
{
    appendLogLines(level, text, false);
}

void MainWindow::appendLogLines(RsyncJob::LogLevel level, const QString &text, bool withDate)
{
    QString color;
    switch (level) {
    case RsyncJob::LogLevel::Error:
        color = QStringLiteral("#e53935");
        break;
    case RsyncJob::LogLevel::Warning:
        color = QStringLiteral("#fb8c00");
        break;
    case RsyncJob::LogLevel::Success:
        color = QStringLiteral("#43a047");
        break;
    case RsyncJob::LogLevel::Info:
    case RsyncJob::LogLevel::Output:
        break;
    }
    // Every line starts with the time (in grey), also every line of a multi-line message. The date is
    // in the line that starts a run and in a line of its own when the day changes (e.g. after midnight).
    const QDateTime now = QDateTime::currentDateTime();
    const QString date = now.date().toString(Qt::ISODate);
    if (m_logDate.isValid() && now.date() != m_logDate && !withDate)
        m_log->appendHtml(QStringLiteral("<span style=\"color:#888888\">--- %1 ---</span>").arg(date));
    m_logDate = now.date();
    QString time = now.time().toString(QStringLiteral("HH:mm:ss"));
    if (withDate)
        time = date + QLatin1Char(' ') + time;
    const QString timeHtml = QStringLiteral("<span style=\"color:#888888\">%1</span> ").arg(time);
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QString escaped = line.toHtmlEscaped();
        m_log->appendHtml(timeHtml + (color.isEmpty() ? escaped
                                                      : QStringLiteral("<span style=\"color:%1\">%2</span>").arg(color, escaped)));
    }
}

void MainWindow::onJobFinished()
{
    const RsyncJob::Summary summary = m_job->summary();
    stopMeasuringSources();
    if (summary.ok())
        m_desktopProgress->finish(false);
    else if (summary.cancelled)
        m_desktopProgress->finish(true);
    else if (summary.differences > 0)
        m_desktopProgress->finish(false, tr("Verification found %1 difference(s).").arg(summary.differences));
    else
        m_desktopProgress->finish(false, tr("Finished with errors."));
    m_progress->setRange(0, 1000);
    m_transferStatus->clear();
    // After a move, sources that no longer exist were transferred and removed - drop them from the list.
    if (currentOptions().removeSourceFiles && !currentOptions().dryRun) {
        for (int i = m_sourceList->count() - 1; i >= 0; --i) {
            const Location loc = Location::parse(m_sourceList->item(i)->data(kLocationRole).toString());
            if (!loc.isRemote() && !LocalPath::info(loc.path).exists)
                delete m_sourceList->takeItem(i);
        }
    }
    refreshSources();
    if (summary.ok() && m_closeWhenFinishedCheck->isChecked() && !m_job->options().dryRun) {
        // A short moment to see the result; unchecking the box in the meantime keeps the window open.
        m_currentFile->setText(tr("Finished successfully. Closing..."));
        QTimer::singleShot(1500, this, [this] {
            if (m_closeWhenFinishedCheck->isChecked() && !(m_job && m_job->isRunning()))
                close();
        });
    } else if (summary.ok()) {
        m_currentFile->setText(tr("Finished successfully."));
        statusBar()->showMessage(tr("Finished successfully."), 10000);
    } else if (summary.cancelled) {
        m_currentFile->setText(tr("Cancelled."));
    } else if (summary.differences > 0) {
        m_currentFile->setText(tr("Verification found differences."));
        QMessageBox box(QMessageBox::Warning, tr("Verification failed"),
                        tr("%1 entr(y/ies) in the destination differ from the source or are missing. "
                           "See the log for details.")
                            .arg(summary.differences),
                        QMessageBox::Ok, this);
        box.setDetailedText(m_job->differingFiles().mid(0, 1000).join(QLatin1Char('\n')));
        box.exec();
    } else {
        m_currentFile->setText(tr("Finished with errors."));
        QMessageBox::warning(this, tr("Finished with errors"),
                             tr("Not everything could be copied. See the log for details."));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_job && m_job->isRunning()) {
        const auto answer = QMessageBox::question(
            this, tr("Copying in progress"),
            tr("A transfer is in progress. Cancel it and quit?\n"
               "The file being copied may be incomplete in the destination."));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        delete m_job; // the destructor kills the rsync process
    }
    stopMeasuringSources(); // quitting waits for running background tasks
    saveSettings();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!(m_job && m_job->isRunning()) && event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QStringList paths = pathsFromMimeData(event->mimeData());
    if (paths.isEmpty())
        return;
    addSources(paths);
    event->acceptProposedAction();
}
