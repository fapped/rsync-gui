#pragma once

#include <QDialog>
#include <QFileDialog>

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;
class QTreeView;
class EntryModel;
class EntrySortModel;

// The file dialogs of the window. The system dialog (KDE's on Plasma) cannot choose names that are not
// valid UTF-8: it replaces their bytes and then reports that the file does not exist. The built-in dialog
// lists directories itself and returns such names exactly; it is used when chosen in the menu next to
// "Add files..." or when preferred for all file dialogs of the window.
namespace FileDialog {

enum class Kind { Preferred, BuiltIn };

// Whether the built-in dialog is used for all file dialogs of the window (a remembered preference).
bool builtInPreferred();
void setBuiltInPreferred(bool on);

// LocalPath strings; empty when cancelled. `options` are those of the system dialog.
QStringList openFiles(QWidget *parent, const QString &title, const QString &dir, Kind kind = Kind::Preferred,
                      QFileDialog::Options options = {});
QString existingDirectory(QWidget *parent, const QString &title, const QString &dir, Kind kind = Kind::Preferred,
                          QFileDialog::Options options = QFileDialog::ShowDirsOnly);

} // namespace FileDialog

class BuiltInFileDialog : public QDialog
{
    Q_OBJECT

public:
    enum Mode { Files, Directory };
    // Returned by exec() when the user asked for the system dialog instead.
    static constexpr int UseSystemDialog = 2;

    BuiltInFileDialog(Mode mode, const QString &title, const QString &dir, QWidget *parent = nullptr);
    ~BuiltInFileDialog() override;

    QString directory() const { return m_dir; }
    void setDirectory(const QString &dir);
    // LocalPath strings: the chosen files, or the chosen directory.
    QStringList selectedPaths() const;
    void selectNames(const QStringList &names);

private:
    void openIndex(const QModelIndex &index);
    void acceptSelection();
    void updateSelection();
    void fillPlaces();
    QString childPath(const QString &name) const;

    Mode m_mode;
    QString m_dir;
    QStringList m_history;
    int m_nonUtf8 = 0;
    QString m_error;
    EntryModel *m_model;
    EntrySortModel *m_sortModel;
    QTreeView *m_view;
    QListWidget *m_places;
    QLineEdit *m_pathEdit;
    QLineEdit *m_nameEdit;
    QFrame *m_infoBox;
    QLabel *m_infoIcon;
    QLabel *m_info;
    QToolButton *m_backButton;
    QToolButton *m_upButton;
    QToolButton *m_hiddenButton;
    QPushButton *m_openButton;
};
