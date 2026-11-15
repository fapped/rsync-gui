#pragma once

#include "networkshares.h"
#include "rsyncoptions.h"

#include <QDialog>
#include <QPointer>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProcess;
class QPushButton;
class QSpinBox;
class RemoteProbe;

// Sets up how one network share is reached over ssh (ShareSettings): the ssh host, the port and the path of the
// share on the server, which can be detected (Samba's testparm) and tested (RemoteProbe) before it is used.
class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    ShareDialog(const NetworkShare &share, const ShareSettings &settings, const RsyncOptions &options,
                QWidget *parent = nullptr);
    ~ShareDialog() override;

    ShareSettings settings() const;

private:
    void detectPath();
    void test();
    void setBusy(bool busy);
    // Runs a command on the server (stdout to `done`, nullptr when ssh failed).
    void runRemote(const QString &command, std::function<void(const QByteArray *output, const QString &errors)> done);

    NetworkShare m_share;
    RsyncOptions m_options;
    ShareSettings::Use m_initialUse;
    QComboBox *m_useCombo = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLineEdit *m_chmodEdit = nullptr;
    QPushButton *m_detectButton = nullptr;
    QPushButton *m_testButton = nullptr;
    QLabel *m_resultLabel = nullptr;
    QPointer<QProcess> m_process;
    QPointer<RemoteProbe> m_probe;
};

// The network shares that are mounted now and those with saved settings: edit (ShareDialog) or forget them.
class SharesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SharesDialog(const RsyncOptions &options, QWidget *parent = nullptr);

private:
    void refresh();
    void editCurrent();
    void forgetCurrent();

    RsyncOptions m_options;
    QListWidget *m_list = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_forgetButton = nullptr;
};
