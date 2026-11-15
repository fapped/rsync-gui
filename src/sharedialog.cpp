#include "sharedialog.h"
#include "localpath.h"
#include "remoteprobe.h"
#include "rsyncjob.h"
#include "sshhosts.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

ShareDialog::ShareDialog(const NetworkShare &share, const ShareSettings &settings, const RsyncOptions &options,
                         QWidget *parent)
    : QDialog(parent), m_share(share), m_options(options), m_initialUse(settings.use)
{
    setWindowTitle(tr("Network share"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("<b>%1</b>%2<br><br>Copies to, from and within this share can use ssh instead of the mount: rsync then "
           "runs on the server (when the source and the destination are both there) or transfers only the changes "
           "over ssh - the data does not go through this computer twice.<br>The ssh account needs a shell and "
           "rsync. Files are then created by the ssh user, not by %3.")
            .arg(share.display().toHtmlEscaped(),
                 share.mountPoint.isEmpty()
                     ? QString()
                     : tr(" (mounted at %1)").arg(LocalPath::display(share.mountPoint).toHtmlEscaped()),
                 share.kind == NetworkShare::Kind::Cifs ? tr("Samba") : tr("sftp")),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    m_useCombo = new QComboBox(this);
    m_useCombo->addItem(tr("Over ssh (rsync on the server when possible)"), int(ShareSettings::Use::Enabled));
    m_useCombo->addItem(tr("Through the mount"), int(ShareSettings::Use::Disabled));
    m_useCombo->addItem(tr("Through the mount, do not ask again"), int(ShareSettings::Use::Never));
    const ShareSettings::Use use = settings.use == ShareSettings::Use::Unset ? ShareSettings::Use::Enabled : settings.use;
    m_useCombo->setCurrentIndex(m_useCombo->findData(int(use)));
    form->addRow(tr("Copy:"), m_useCombo);

    m_targetEdit = new QLineEdit(settings.sshTarget, this);
    m_targetEdit->setPlaceholderText(tr("user@host or a Host from ~/.ssh/config"));
    form->addRow(tr("ssh login:"), m_targetEdit);
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(0, 65535);
    m_portSpin->setSpecialValueText(tr("default"));
    m_portSpin->setValue(settings.port);
    form->addRow(tr("ssh port:"), m_portSpin);

    auto *pathRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit(settings.serverPath, this);
    m_pathEdit->setPlaceholderText(tr("e.g. /volume1/video"));
    m_pathEdit->setToolTip(tr("The directory on the server that is mounted at %1.").arg(LocalPath::display(share.mountPoint)));
    pathRow->addWidget(m_pathEdit, 1);
    m_detectButton = new QPushButton(tr("Detect"), this);
    m_detectButton->setToolTip(tr("Asks Samba on the server (testparm) for the path of the share."));
    m_detectButton->setVisible(share.kind == NetworkShare::Kind::Cifs);
    pathRow->addWidget(m_detectButton);
    form->addRow(tr("Path on the server:"), pathRow);
    m_chmodEdit = new QLineEdit(settings.chmod, this);
    m_chmodEdit->setPlaceholderText(tr("e.g. Fg-wx,Fo-wx (empty: the modes of the sources)"));
    m_chmodEdit->setToolTip(tr("rsync --chmod for copies to this share over ssh, so that files get the modes Samba "
                               "would give them (Detect fills it from the share's create mask / directory mask). "
                               "Empty: the modes are copied from the sources (-p)."));
    form->addRow(tr("Modes (--chmod):"), m_chmodEdit);
    layout->addLayout(form);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_resultLabel);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_testButton = buttons->addButton(tr("Test"), QDialogButtonBox::ActionRole);
    m_testButton->setToolTip(tr("Logs in over ssh and checks that rsync can run there and the path is writable."));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        const ShareSettings s = this->settings();
        if (s.use == ShareSettings::Use::Enabled && (s.sshTarget.isEmpty() || s.serverPath.isEmpty())) {
            QMessageBox::warning(this, windowTitle(), tr("Enter the ssh login and the path of the share on the server."));
            return;
        }
        accept();
    });
    connect(m_detectButton, &QPushButton::clicked, this, &ShareDialog::detectPath);
    connect(m_testButton, &QPushButton::clicked, this, &ShareDialog::test);
    resize(560, sizeHint().height());
}

ShareDialog::~ShareDialog()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

ShareSettings ShareDialog::settings() const
{
    ShareSettings s;
    s.use = ShareSettings::Use(m_useCombo->currentData().toInt());
    s.sshTarget = m_targetEdit->text().trimmed();
    s.port = m_portSpin->value();
    s.serverPath = m_pathEdit->text().trimmed();
    s.chmod = m_chmodEdit->text().trimmed();
    if (s.chmod.startsWith(QStringLiteral("--chmod=")))
        s.chmod = s.chmod.mid(8);
    return s;
}

void ShareDialog::setBusy(bool busy)
{
    m_detectButton->setEnabled(!busy);
    m_testButton->setEnabled(!busy);
    if (busy)
        m_resultLabel->setText(tr("Connecting..."));
}

void ShareDialog::runRemote(const QString &command, std::function<void(const QByteArray *, const QString &)> done)
{
    QStringList ssh = m_options.sshCommand(m_portSpin->value());
    const QString program = ssh.takeFirst();
    ssh << QStringLiteral("--") << m_targetEdit->text().trimmed() << command;
    auto *process = new QProcess(this);
    process->setStandardInputFile(QProcess::nullDevice());
    m_process = process;
    connect(process, &QProcess::finished, this, [process, done](int exitCode, QProcess::ExitStatus status) {
        process->deleteLater();
        const QByteArray output = process->readAllStandardOutput();
        const QString errors = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        done(status == QProcess::NormalExit && exitCode != 255 ? &output : nullptr, errors);
    });
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            done(nullptr, process->errorString());
        }
    });
    RsyncJob::setRawArguments(process, program, ssh);
    process->start(program, ssh);
}

void ShareDialog::detectPath()
{
    if (m_targetEdit->text().trimmed().isEmpty())
        return;
    setBusy(true);
    runRemote(NetworkShare::sharePathCommand(m_share.share) + QStringLiteral("; echo; ")
                  + NetworkShare::ownershipCommand(m_share.share),
              [this](const QByteArray *output, const QString &errors) {
                  setBusy(false);
                  if (!output) {
                      m_resultLabel->setText(tr("ssh could not connect: %1").arg(errors));
                      return;
                  }
                  const QStringList lines = QString::fromLocal8Bit(*output).split(QLatin1Char('\n'));
                  const QString path = lines.value(0).trimmed();
                  if (!path.startsWith(QLatin1Char('/'))) {
                      m_resultLabel->setText(tr("Samba on the server did not report the path of \"%1\" (no testparm, or "
                                                "the configuration cannot be read) - enter it by hand.%2")
                                                 .arg(m_share.share, errors.isEmpty() ? QString() : QStringLiteral("\n") + errors));
                      return;
                  }
                  QString full = path;
                  if (!m_share.remotePath.isEmpty())
                      full += QLatin1Char('/') + m_share.remotePath;
                  m_pathEdit->setText(full);
                  // The modes Samba gives new files -> --chmod; the owner it forces cannot be copied over ssh.
                  const QMap<QString, QString> parameters =
                      NetworkShare::parseParameters(lines.mid(1).join(QLatin1Char('\n')).toLocal8Bit());
                  const QString chmod = NetworkShare::chmodFromSamba(parameters);
                  m_chmodEdit->setText(chmod);
                  QString text = tr("The share \"%1\" is %2 on the server.").arg(m_share.share, path);
                  QStringList modes;
                  for (const char *name : {"create mask", "force create mode", "directory mask", "force directory mode"}) {
                      if (parameters.contains(QLatin1String(name)))
                          modes << QStringLiteral("%1 = %2").arg(QLatin1String(name), parameters.value(QLatin1String(name)));
                  }
                  if (!modes.isEmpty())
                      text += tr("\nSamba: %1 - the same modes over ssh: %2")
                                  .arg(modes.join(QStringLiteral(", ")), chmod.isEmpty() ? tr("nothing to change") : QStringLiteral("--chmod=") + chmod);
                  QStringList owners;
                  for (const char *name : {"force user", "force group"}) {
                      if (parameters.contains(QLatin1String(name)))
                          owners << QStringLiteral("%1 = %2").arg(QLatin1String(name), parameters.value(QLatin1String(name)));
                  }
                  if (!owners.isEmpty())
                      text += tr("\nSamba creates files with %1 - over ssh they belong to the ssh user (rsync cannot "
                                 "change the owner without root).")
                                  .arg(owners.join(QStringLiteral(", ")));
                  m_resultLabel->setText(text);
              });
}

void ShareDialog::test()
{
    const ShareSettings s = settings();
    if (s.sshTarget.isEmpty() || s.serverPath.isEmpty()) {
        m_resultLabel->setText(tr("Enter the ssh login and the path of the share on the server."));
        return;
    }
    setBusy(true);
    auto *probe = new RemoteProbe(m_options, s.sshTarget, s.port, {}, s.serverPath, this);
    m_probe = probe;
    connect(probe, &RemoteProbe::finished, this, [this, probe](const RemoteProbe::Result &result) {
        probe->deleteLater();
        setBusy(false);
        if (const QString error = result.error(); !error.isEmpty()) {
            m_resultLabel->setText(error);
            return;
        }
        QString text;
        if (result.destination.type != RemoteProbe::Type::Directory)
            text = tr("The path does not exist on the server (or is not a directory).");
        else if (!result.parentWritable)
            text = tr("%1 - but the ssh user cannot write to the path.").arg(result.rsyncVersion);
        else
            text = tr("OK: %1, %2 free.").arg(result.rsyncVersion, QLocale().formattedDataSize(result.freeBytes));
        m_resultLabel->setText(text);
    });
    probe->start();
}

SharesDialog::SharesDialog(const RsyncOptions &options, QWidget *parent) : QDialog(parent), m_options(options)
{
    setWindowTitle(tr("Network shares"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Mounted network shares (CIFS/SMB, sshfs) and those set up before. A share set up for "
                                "ssh is copied over ssh, or by rsync on the server when the source and the destination "
                                "are both there (option \"Run rsync on the server\")."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_editButton = buttons->addButton(tr("Set up..."), QDialogButtonBox::ActionRole);
    m_forgetButton = buttons->addButton(tr("Forget"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_editButton, &QPushButton::clicked, this, &SharesDialog::editCurrent);
    connect(m_forgetButton, &QPushButton::clicked, this, &SharesDialog::forgetCurrent);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &SharesDialog::editCurrent);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        m_editButton->setEnabled(row >= 0);
        m_forgetButton->setEnabled(row >= 0);
    });
    refresh();
    resize(620, 360);
}

void SharesDialog::refresh()
{
    m_list->clear();
    QSettings qsettings;
    QStringList keys;
    QList<NetworkShare> shares;
    for (const Mount &mount : Mount::current()) {
        const std::optional<NetworkShare> share = NetworkShare::fromMount(mount);
        if (share && !keys.contains(share->key)) {
            keys << share->key;
            shares << *share;
        }
    }
    for (const QString &key : ShareSettings::savedKeys(qsettings)) {
        if (!keys.contains(key)) {
            keys << key;
            shares << NetworkShare::fromKey(key);
        }
    }
    for (const NetworkShare &share : std::as_const(shares)) {
        const ShareSettings s = ShareSettings::load(qsettings, share.key);
        QString state;
        switch (s.use) {
        case ShareSettings::Use::Enabled: state = tr("over ssh: %1").arg(s.location(QString()).toString()); break;
        case ShareSettings::Use::Disabled: state = tr("through the mount"); break;
        case ShareSettings::Use::Never: state = tr("through the mount, not asked"); break;
        case ShareSettings::Use::Unset: state = tr("not set up"); break;
        }
        const QString where = share.mountPoint.isEmpty() ? tr("not mounted") : LocalPath::display(share.mountPoint);
        auto *item = new QListWidgetItem(QStringLiteral("%1  (%2)  -  %3").arg(share.display(), where, state), m_list);
        item->setData(Qt::UserRole, share.key);
        item->setData(Qt::UserRole + 1, share.mountPoint);
    }
    m_editButton->setEnabled(false);
    m_forgetButton->setEnabled(false);
}

void SharesDialog::editCurrent()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString key = item->data(Qt::UserRole).toString();
    NetworkShare share = NetworkShare::fromKey(key);
    for (const Mount &mount : Mount::current()) {
        const std::optional<NetworkShare> mounted = NetworkShare::fromMount(mount);
        if (mounted && mounted->key == key)
            share = *mounted;
    }
    QSettings qsettings;
    ShareSettings settings = ShareSettings::load(qsettings, key);
    if (settings.sshTarget.isEmpty() && settings.serverPath.isEmpty()) {
        const ShareSettings detected = ShareSettings::detected(share);
        // An alias from ~/.ssh/config that leads to the server, if there is one (it carries the user, the port, the key).
        settings.sshTarget = SshHosts::suggestTarget(m_options.sshCommand(0), share.server, share.user, {},
                                                     [this](const Location &loc) {
                                                         return SshHosts::resolve(m_options.sshCommand(0),
                                                                                  loc.user.isEmpty() ? loc.host : loc.user + QLatin1Char('@') + loc.host,
                                                                                  loc.port);
                                                     });
        if (share.kind == NetworkShare::Kind::Sshfs)
            settings.sshTarget = detected.sshTarget; // the mount names its login itself
        settings.serverPath = detected.serverPath;
    }
    ShareDialog dialog(share, settings, m_options, this);
    if (dialog.exec() == QDialog::Accepted) {
        dialog.settings().save(qsettings, key);
        refresh();
    }
}

void SharesDialog::forgetCurrent()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    QSettings qsettings;
    ShareSettings::remove(qsettings, item->data(Qt::UserRole).toString());
    refresh();
}
