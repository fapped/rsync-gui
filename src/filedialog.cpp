#include "filedialog.h"

#include "filelisting.h"
#include "localpath.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QCollator>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMimeDatabase>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>
#include <cstring>

namespace {

const QString kPreferredKey = QStringLiteral("window/builtInFileDialog");
const QString kGeometryKey = QStringLiteral("window/builtInFileDialogGeometry");
const QString kHiddenKey = QStringLiteral("window/builtInFileDialogShowHidden");

constexpr int NameRole = Qt::UserRole;       // the LocalPath name
constexpr int IsDirRole = Qt::UserRole + 1;

// Breeze's "neutral" text colour, for the bytes that are not valid UTF-8.
const QColor kNeutral(0xf6, 0x74, 0x00);

// The deepest directory of `dir` (itself included) that the system dialog can open: one whose path is
// valid UTF-8.
QString systemDialogStart(QString dir)
{
    while (!dir.isEmpty() && dir != QLatin1String("/") && (!LocalPath::isPlain(dir) || !LocalPath::isDirectory(dir)))
        dir = dir.left(qMax<qsizetype>(1, dir.lastIndexOf(QLatin1Char('/'))));
    return dir.isEmpty() ? QDir::homePath() : dir;
}

} // namespace

class EntryModel : public QAbstractTableModel
{
public:
    using QAbstractTableModel::QAbstractTableModel;

    bool dirsOnly = false;
    bool showHidden = false;

    // Returns errno of reading the directory (0 on success).
    int load(const QString &dir)
    {
        beginResetModel();
        int error = 0;
        m_entries.clear();
        for (const FileListing::Entry &entry : FileListing::list(dir, &error)) {
            if ((dirsOnly && !entry.isDir) || (!showHidden && entry.name.startsWith(QLatin1Char('.'))))
                continue;
            m_entries << entry;
        }
        endResetModel();
        return error;
    }

    int nonUtf8Count() const
    {
        return int(std::count_if(m_entries.cbegin(), m_entries.cend(),
                                 [](const FileListing::Entry &e) { return !LocalPath::isPlain(e.name); }));
    }

    const FileListing::Entry &entry(int row) const { return m_entries.at(row); }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_entries.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
            return {};
        switch (section) {
        case 0:
            return BuiltInFileDialog::tr("Name");
        case 1:
            return BuiltInFileDialog::tr("Size");
        default:
            return BuiltInFileDialog::tr("Modified");
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        const FileListing::Entry &e = m_entries.at(index.row());
        switch (role) {
        case NameRole:
            return e.name;
        case IsDirRole:
            return e.isDir;
        case Qt::ToolTipRole:
            if (index.column() == 0 && !LocalPath::isPlain(e.name))
                return BuiltInFileDialog::tr("This name is not valid UTF-8: the bytes %1 are shown as \\xNN, the name "
                                             "is kept exactly.\nRead as Latin-1: %2")
                    .arg(FileListing::invalidBytes(e.name), FileListing::latin1Reading(e.name));
            return {};
        case Qt::DecorationRole:
            if (index.column() != 0)
                return {};
            if (e.isDir)
                return QIcon::fromTheme(QStringLiteral("folder"));
            return QIcon::fromTheme(mimeDatabase().mimeTypeForFile(FileListing::latin1Reading(e.name),
                                                                   QMimeDatabase::MatchExtension).iconName(),
                                    QIcon::fromTheme(QStringLiteral("unknown")));
        case Qt::FontRole:
            if (index.column() == 0 && e.isSymLink) {
                QFont italic;
                italic.setItalic(true);
                return italic;
            }
            return {};
        case Qt::TextAlignmentRole:
            if (index.column() == 1)
                return QVariant(Qt::AlignRight | Qt::AlignVCenter);
            return {};
        case Qt::DisplayRole:
            switch (index.column()) {
            case 0:
                return LocalPath::display(e.name);
            case 1:
                return e.isDir ? QString() : QLocale().formattedDataSize(e.size);
            default:
                return QLocale().toString(QDateTime::fromSecsSinceEpoch(e.modified), QLocale::ShortFormat);
            }
        default:
            return {};
        }
    }

private:
    static QMimeDatabase &mimeDatabase()
    {
        static QMimeDatabase db;
        return db;
    }

    QList<FileListing::Entry> m_entries;
};

// Directories first (in both orders), then by the column; names in natural order ("2" before "10").
class EntrySortModel : public QSortFilterProxyModel
{
public:
    explicit EntrySortModel(EntryModel *model, QObject *parent)
        : QSortFilterProxyModel(parent)
        , m_model(model)
    {
        m_collator.setNumericMode(true);
        m_collator.setCaseSensitivity(Qt::CaseInsensitive);
        setSourceModel(model);
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const FileListing::Entry &a = m_model->entry(left.row());
        const FileListing::Entry &b = m_model->entry(right.row());
        if (a.isDir != b.isDir)
            return (sortOrder() == Qt::AscendingOrder) == a.isDir;
        if (left.column() == 1 && a.size != b.size)
            return a.size < b.size;
        if (left.column() == 2 && a.modified != b.modified)
            return a.modified < b.modified;
        return m_collator.compare(LocalPath::display(a.name), LocalPath::display(b.name)) < 0;
    }

private:
    EntryModel *m_model;
    QCollator m_collator;
};

// Draws the bytes of a name that are not valid UTF-8 as small "\xe9" badges.
class NameDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString name = index.data(NameRole).toString();
        if (index.column() != 0 || LocalPath::isPlain(name)) {
            QStyledItemDelegate::paint(p, option, index);
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, widget);

        const QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(2, 0, -2, 0);
        const bool selected = opt.state & QStyle::State_Selected;
        const QColor text = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        const bool dark = opt.palette.color(QPalette::Base).lightness() < 128;
        QFont badgeFont = opt.font;
        badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.85);
        const QFontMetrics fm(opt.font);
        const QFontMetrics bm(badgeFont);
        const int baseline = r.top() + (r.height() + fm.ascent() - fm.descent()) / 2;

        p->save();
        p->setClipRect(r);
        p->setRenderHint(QPainter::Antialiasing);
        int x = r.left();
        QString run;
        const auto flush = [&] {
            if (run.isEmpty())
                return;
            p->setFont(opt.font);
            p->setPen(text);
            p->drawText(x, baseline, run);
            x += fm.horizontalAdvance(run);
            run.clear();
        };
        for (qsizetype i = 0; i < name.size(); ++i) {
            const QString single(name.at(i));
            if (LocalPath::isPlain(single)) {
                run += name.at(i);
                continue;
            }
            flush();
            const QString badge = LocalPath::display(single);
            const QRect box(x, r.top() + (r.height() - bm.height()) / 2 - 1, bm.horizontalAdvance(badge) + 6,
                            bm.height() + 2);
            QColor fill = selected ? opt.palette.color(QPalette::HighlightedText) : kNeutral;
            fill.setAlphaF(selected ? 0.25f : 0.16f);
            p->setPen(Qt::NoPen);
            p->setBrush(fill);
            p->drawRoundedRect(box, 3, 3);
            p->setFont(badgeFont);
            p->setPen(selected ? text : (dark ? kNeutral.lighter(115) : kNeutral.darker(130)));
            p->drawText(box, Qt::AlignCenter, badge);
            x = box.right() + 1;
        }
        flush();
        p->restore();
    }
};

namespace FileDialog {

bool builtInPreferred()
{
    return QSettings().value(kPreferredKey, false).toBool();
}

void setBuiltInPreferred(bool on)
{
    QSettings().setValue(kPreferredKey, on);
}

QStringList openFiles(QWidget *parent, const QString &title, const QString &dir, Kind kind, QFileDialog::Options options)
{
    QString start = dir;
    if (kind == Kind::BuiltIn || builtInPreferred()) {
        BuiltInFileDialog dialog(BuiltInFileDialog::Files, title, dir, parent);
        const int result = dialog.exec();
        if (result == QDialog::Accepted)
            return dialog.selectedPaths();
        if (result != BuiltInFileDialog::UseSystemDialog)
            return {};
        start = dialog.directory();
    }
    return QFileDialog::getOpenFileNames(parent, title, systemDialogStart(start), QString(), nullptr, options);
}

QString existingDirectory(QWidget *parent, const QString &title, const QString &dir, Kind kind,
                          QFileDialog::Options options)
{
    QString start = dir;
    if (kind == Kind::BuiltIn || builtInPreferred()) {
        BuiltInFileDialog dialog(BuiltInFileDialog::Directory, title, dir, parent);
        const int result = dialog.exec();
        if (result == QDialog::Accepted)
            return dialog.selectedPaths().value(0);
        if (result != BuiltInFileDialog::UseSystemDialog)
            return {};
        start = dialog.directory();
    }
    return QFileDialog::getExistingDirectory(parent, title, systemDialogStart(start), options);
}

} // namespace FileDialog

BuiltInFileDialog::BuiltInFileDialog(Mode mode, const QString &title, const QString &dir, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_model(new EntryModel(this))
    , m_sortModel(new EntrySortModel(m_model, this))
{
    setWindowTitle(title);
    m_model->dirsOnly = mode == Directory;
    m_model->showHidden = QSettings().value(kHiddenKey, false).toBool();

    m_backButton = new QToolButton(this);
    m_backButton->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    m_backButton->setToolTip(tr("Back"));
    m_upButton = new QToolButton(this);
    m_upButton->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
    m_upButton->setToolTip(tr("Parent directory"));
    m_backButton->setShortcut(QKeySequence::Back);
    m_upButton->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setToolTip(tr("Type a directory and press Enter. Bytes that are not valid UTF-8 can be typed as \\xNN."));
    m_hiddenButton = new QToolButton(this);
    m_hiddenButton->setIcon(QIcon::fromTheme(QStringLiteral("view-hidden")));
    m_hiddenButton->setToolTip(tr("Show hidden files"));
    m_hiddenButton->setCheckable(true);
    m_hiddenButton->setChecked(m_model->showHidden);
    auto *top = new QHBoxLayout;
    top->addWidget(m_backButton);
    top->addWidget(m_upButton);
    top->addWidget(m_pathEdit, 1);
    top->addWidget(m_hiddenButton);

    m_places = new QListWidget(this);
    m_places->setIconSize(QSize(22, 22));
    m_places->setFrameShape(QFrame::NoFrame);
    m_places->setBackgroundRole(QPalette::Window);
    fillPlaces();

    m_view = new QTreeView(this);
    m_view->setModel(m_sortModel);
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setUniformRowHeights(true);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->setSelectionMode(mode == Files ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setItemDelegate(new NameDelegate(m_view));
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->header()->resizeSection(1, 90);
    m_view->header()->resizeSection(2, 140);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_places);
    splitter->addWidget(m_view);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({170, 600});

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setReadOnly(true);
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(mode == Files ? tr("Name:") : tr("Directory:"), this));
    nameRow->addWidget(m_nameEdit, 1);

    m_infoBox = new QFrame(this);
    m_infoIcon = new QLabel(m_infoBox);
    m_infoIcon->setAlignment(Qt::AlignTop);
    m_info = new QLabel(m_infoBox);
    m_info->setWordWrap(true);
    m_info->setTextFormat(Qt::RichText);
    auto *infoLayout = new QHBoxLayout(m_infoBox);
    infoLayout->setContentsMargins(4, 2, 4, 2);
    infoLayout->addWidget(m_infoIcon);
    infoLayout->addWidget(m_info, 1);

    auto *systemButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-open-folder")),
                                         tr("System dialog..."), this);
    systemButton->setToolTip(tr("Choose in the dialog of the desktop instead (it cannot choose names that are "
                                "not valid UTF-8)"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
    m_openButton = buttons->button(QDialogButtonBox::Open);
    if (mode == Directory)
        m_openButton->setText(tr("Select"));
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(systemButton);
    bottom->addStretch();
    bottom->addWidget(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(splitter, 1);
    layout->addLayout(nameRow);
    layout->addWidget(m_infoBox);
    layout->addLayout(bottom);

    connect(m_backButton, &QToolButton::clicked, this, [this] {
        if (m_history.isEmpty())
            return;
        const QString dir = m_history.takeLast();
        const QString current = m_dir;
        m_dir.clear(); // not added to the history
        setDirectory(dir);
        selectNames({current.mid(current.lastIndexOf(QLatin1Char('/')) + 1)});
    });
    connect(m_upButton, &QToolButton::clicked, this, [this] {
        const QString current = m_dir;
        const qsizetype slash = m_dir.lastIndexOf(QLatin1Char('/'));
        setDirectory(slash <= 0 ? QStringLiteral("/") : m_dir.left(slash));
        selectNames({current.mid(slash + 1)});
    });
    connect(m_hiddenButton, &QToolButton::toggled, this, [this](bool on) {
        m_model->showHidden = on;
        QSettings().setValue(kHiddenKey, on);
        const QString dir = m_dir;
        m_dir.clear();
        setDirectory(dir);
    });
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this] {
        QString path = m_pathEdit->text().trimmed();
        if (!LocalPath::isDirectory(path))
            path = FileListing::fromDisplay(path);
        if (!path.startsWith(QLatin1Char('/')))
            path = childPath(path);
        if (LocalPath::isDirectory(path)) {
            setDirectory(QDir::cleanPath(path));
        } else {
            m_error = tr("%1 is not a directory.").arg(LocalPath::display(path));
            updateSelection();
        }
    });
    connect(m_places, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (const QString dir = item->data(Qt::UserRole).toString(); !dir.isEmpty())
            setDirectory(dir);
    });
    connect(m_view, &QTreeView::activated, this, &BuiltInFileDialog::openIndex);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &BuiltInFileDialog::updateSelection);
    connect(buttons, &QDialogButtonBox::accepted, this, &BuiltInFileDialog::acceptSelection);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(systemButton, &QPushButton::clicked, this, [this] { done(UseSystemDialog); });

    resize(820, 520);
    restoreGeometry(QSettings().value(kGeometryKey).toByteArray());
    setDirectory(LocalPath::isDirectory(dir) ? dir : QDir::homePath());
    m_view->setFocus();
}

BuiltInFileDialog::~BuiltInFileDialog()
{
    QSettings().setValue(kGeometryKey, saveGeometry());
}

void BuiltInFileDialog::fillPlaces()
{
    const auto add = [this](const QString &icon, const QString &label, const QString &path) {
        auto *item = new QListWidgetItem(QIcon::fromTheme(icon), label, m_places);
        item->setData(Qt::UserRole, path);
        item->setToolTip(LocalPath::display(path));
    };
    const auto section = [this](const QString &label) {
        auto *item = new QListWidgetItem(label, m_places);
        item->setFlags(Qt::NoItemFlags);
        QFont font = item->font();
        font.setBold(true);
        font.setPointSizeF(font.pointSizeF() * 0.9);
        item->setFont(font);
    };
    section(tr("Places"));
    add(QStringLiteral("user-home"), tr("Home"), QDir::homePath());
    const struct
    {
        QStandardPaths::StandardLocation location;
        const char *icon;
    } standard[] = {
        {QStandardPaths::DesktopLocation, "user-desktop"},
        {QStandardPaths::DocumentsLocation, "folder-documents"},
        {QStandardPaths::DownloadLocation, "folder-download"},
        {QStandardPaths::PicturesLocation, "folder-pictures"},
    };
    for (const auto &place : standard) {
        const QString path = QStandardPaths::writableLocation(place.location);
        if (path != QDir::homePath() && LocalPath::isDirectory(path))
            add(QLatin1String(place.icon), QStandardPaths::displayName(place.location), path);
    }
    add(QStringLiteral("folder-root"), tr("Root"), QStringLiteral("/"));

    QFile mounts(QStringLiteral("/proc/self/mounts"));
    QStringList devices;
    if (mounts.open(QIODevice::ReadOnly)) {
        for (const QString &point : FileListing::mountPoints(mounts.readAll())) {
            if ((point.startsWith(QLatin1String("/media/")) || point.startsWith(QLatin1String("/run/media/"))
                 || point.startsWith(QLatin1String("/mnt/")))
                && !devices.contains(point) && LocalPath::isDirectory(point))
                devices << point;
        }
    }
    if (!devices.isEmpty()) {
        section(tr("Devices"));
        for (const QString &device : std::as_const(devices))
            add(QStringLiteral("drive-removable-media"), LocalPath::display(device.section(QLatin1Char('/'), -1)), device);
    }
}

QString BuiltInFileDialog::childPath(const QString &name) const
{
    return m_dir.endsWith(QLatin1Char('/')) ? m_dir + name : m_dir + QLatin1Char('/') + name;
}

void BuiltInFileDialog::setDirectory(const QString &dir)
{
    if (!m_dir.isEmpty() && dir != m_dir)
        m_history << m_dir;
    m_dir = dir;
    const int error = m_model->load(dir);
    m_error = error ? tr("The directory cannot be read: %1").arg(QString::fromLocal8Bit(std::strerror(error)))
                    : QString();
    m_nonUtf8 = m_model->nonUtf8Count();
    m_pathEdit->setText(LocalPath::display(dir));
    m_backButton->setEnabled(!m_history.isEmpty());
    m_upButton->setEnabled(dir != QLatin1String("/"));
    m_places->clearSelection();
    for (int i = 0; i < m_places->count(); ++i) {
        if (m_places->item(i)->data(Qt::UserRole).toString() == dir)
            m_places->item(i)->setSelected(true);
    }
    if (m_sortModel->rowCount() > 0)
        m_view->selectionModel()->setCurrentIndex(m_sortModel->index(0, 0), QItemSelectionModel::NoUpdate);
    updateSelection();
}

void BuiltInFileDialog::selectNames(const QStringList &names)
{
    m_view->selectionModel()->clearSelection();
    for (int row = 0; row < m_sortModel->rowCount(); ++row) {
        const QModelIndex index = m_sortModel->index(row, 0);
        if (names.contains(index.data(NameRole).toString())) {
            m_view->selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
            m_view->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
            m_view->scrollTo(index);
        }
    }
}

void BuiltInFileDialog::openIndex(const QModelIndex &index)
{
    if (index.data(IsDirRole).toBool())
        setDirectory(childPath(index.data(NameRole).toString()));
    else
        acceptSelection();
}

QStringList BuiltInFileDialog::selectedPaths() const
{
    QStringList paths;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows()) {
        if (m_mode == Directory || !index.data(IsDirRole).toBool())
            paths << childPath(index.data(NameRole).toString());
    }
    if (m_mode == Directory && paths.isEmpty())
        paths << m_dir;
    return paths;
}

void BuiltInFileDialog::acceptSelection()
{
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    // Open with only a directory selected goes into it, like in other file dialogs.
    if (m_mode == Files && rows.size() == 1 && rows.first().data(IsDirRole).toBool()) {
        openIndex(rows.first());
        return;
    }
    if (!selectedPaths().isEmpty())
        accept();
}

void BuiltInFileDialog::updateSelection()
{
    QStringList shown;
    QString single;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        const QString name = index.data(NameRole).toString();
        if (m_mode == Files && index.data(IsDirRole).toBool())
            continue;
        shown << LocalPath::display(name);
        if (!LocalPath::isPlain(name))
            single = rows.size() == 1 ? name : QString();
    }
    if (m_mode == Directory && shown.isEmpty())
        shown << LocalPath::display(m_dir);
    if (shown.size() == 1) {
        m_nameEdit->setText(shown.first());
    } else {
        for (QString &name : shown)
            name = QLatin1Char('"') + name + QLatin1Char('"');
        m_nameEdit->setText(shown.join(QLatin1Char(' ')));
    }
    m_openButton->setEnabled(!rows.isEmpty() || m_mode == Directory);

    QString text;
    QString icon = QStringLiteral("dialog-information");
    if (!m_error.isEmpty()) {
        text = m_error.toHtmlEscaped();
        icon = QStringLiteral("dialog-warning");
    } else if (!single.isEmpty()) {
        const QString bytes = FileListing::invalidBytes(single);
        text = (bytes.contains(QLatin1Char(' ')) ? tr("<b>%1</b>: the bytes %2 are not valid UTF-8.")
                                                 : tr("<b>%1</b>: the byte %2 is not valid UTF-8."))
                   .arg(LocalPath::display(single).toHtmlEscaped(), bytes)
               + QLatin1Char(' ')
               + tr("The name is kept exactly; read as Latin-1 it would be <b>%1</b>.")
                     .arg(FileListing::latin1Reading(single).toHtmlEscaped());
    } else if (m_nonUtf8 > 0) {
        text = (m_nonUtf8 == 1 ? tr("1 name here contains bytes that are not valid UTF-8.")
                               : tr("%1 names here contain bytes that are not valid UTF-8.").arg(m_nonUtf8))
               + QLatin1Char(' ') + tr("They are shown as \\xNN, and the names are kept exactly.");
    }
    m_info->setText(text);
    m_infoIcon->setPixmap(QIcon::fromTheme(icon).pixmap(style()->pixelMetric(QStyle::PM_SmallIconSize)));
    m_infoBox->setVisible(!text.isEmpty());
}
