// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagebrowserwidget.h"
#include "dwarfsctljob.h"
#include "backendquery.h"
#include <KLocalizedString>
#include <QAction>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QAbstractTableModel>
#include <QIcon>
#include <QVector>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <cmath>
#include <sys/stat.h>

namespace {
constexpr int PathRole = Qt::UserRole + 1;
constexpr int TypeRole = Qt::UserRole + 2;
constexpr int SizeRole = Qt::UserRole + 3;
class EntrySort final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    bool lessThan(const QModelIndex &a, const QModelIndex &b) const override
    {
        const bool ad = a.siblingAtColumn(0).data(TypeRole).toUInt() == S_IFDIR;
        const bool bd = b.siblingAtColumn(0).data(TypeRole).toUInt() == S_IFDIR;
        if (ad != bd) return sortOrder() == Qt::AscendingOrder ? ad : bd;
        if (a.column() == 1) {
            const auto as = a.data(SizeRole), bs = b.data(SizeRole);
            if (as != bs) return as.toULongLong() < bs.toULongLong();
        }
        return QString::localeAwareCompare(a.data().toString(), b.data().toString()) < 0;
    }
};
bool integer(const QJsonValue &value, double limit)
{
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 0 && number <= limit && std::floor(number) == number;
}
bool validPath(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar::Null)) return false;
    if (path == QLatin1String("/")) return true;
    const auto parts = path.mid(1).split(QLatin1Char('/'));
    for (const auto &part : parts)
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) return false;
    return true;
}
QString parentPath(const QString &path)
{
    const auto slash = path.lastIndexOf(QLatin1Char('/'));
    return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}
}

// One compact record per entry; a reset publishes the validated directory in
// one notification rather than allocating and inserting three items per row.
class ImageEntryModel final : public QAbstractTableModel {
public:
    struct Entry { QString name, path, sizeText; quint32 type; qint64 size; };
    explicit ImageEntryModel(QObject *parent) : QAbstractTableModel(parent),
        m_folder(QIcon::fromTheme(QStringLiteral("folder"))),
        m_link(QIcon::fromTheme(QStringLiteral("emblem-symbolic-link"))),
        m_file(QIcon::fromTheme(QStringLiteral("text-x-generic"))) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_entries.size(); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
            if (section == 0) return i18n("Name");
            if (section == 1) return i18n("Size");
            if (section == 2) return i18n("Type");
        }
        return {};
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) return {};
        const auto &entry = m_entries.at(index.row());
        if (role == PathRole && index.column() == 0) return entry.path;
        if (role == TypeRole && index.column() == 0) return entry.type;
        if (role == SizeRole && index.column() == 1 && entry.size >= 0) return entry.size;
        if (role == Qt::TextAlignmentRole && index.column() == 1) return int(Qt::AlignRight | Qt::AlignVCenter);
        if (role == Qt::DecorationRole && index.column() == 0)
            return entry.type == S_IFDIR ? m_folder : entry.type == S_IFLNK ? m_link : m_file;
        if (role == Qt::DisplayRole) {
            if (index.column() == 0) return entry.name;
            if (index.column() == 1) return entry.sizeText;
            if (index.column() == 2) return entry.type == S_IFDIR ? i18n("Folder") : entry.type == S_IFREG ? i18n("File") : entry.type == S_IFLNK ? i18n("Symbolic link") : i18n("Special file");
        }
        return {};
    }
    void replace(QVector<Entry> entries = {}) {
        beginResetModel(); m_entries = std::move(entries); endResetModel();
    }
private:
    QVector<Entry> m_entries;
    QIcon m_folder, m_link, m_file;
};

ImageBrowserWidget::ImageBrowserWidget(QWidget *parent) : QWidget(parent)
{
    m_readRetry = new dwarfsplugin::BackendReadRetry(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QHBoxLayout;
    m_back = new QToolButton(this);
    m_back->setObjectName(QStringLiteral("imageBrowserBack"));
    m_back->setIcon(QIcon::fromTheme(QStringLiteral("go-previous")));
    m_back->setToolTip(i18n("Back"));
    m_up = new QToolButton(this);
    m_up->setObjectName(QStringLiteral("imageBrowserUp"));
    m_up->setIcon(QIcon::fromTheme(QStringLiteral("go-up")));
    m_up->setToolTip(i18n("Up"));
    m_path = new QLineEdit(this);
    m_path->setObjectName(QStringLiteral("imageBrowserPath"));
    m_path->setReadOnly(true);
    m_path->setAccessibleName(i18n("Directory in image"));
    m_path->setText(m_directory);
    m_extract = new QToolButton(this);
    m_extract->setObjectName(QStringLiteral("extractSelectedButton"));
    m_extract->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_extract->setText(i18n("Extract Selected…"));
    m_extract->setEnabled(false);
    bar->addWidget(m_back);
    bar->addWidget(m_up);
    bar->addWidget(m_path, 1);
    bar->addWidget(m_extract);
    layout->addLayout(bar);
    m_state = new QLabel(i18n("Select an image to browse its files."), this);
    m_state->setObjectName(QStringLiteral("imageBrowserState"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    m_state->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_retry = new QToolButton(this);
    m_retry->setObjectName(QStringLiteral("imageBrowserRetry"));
    m_retry->setText(i18n("Retry"));
    m_retry->hide();
    auto *stateBar = new QHBoxLayout;
    stateBar->addWidget(m_state, 1);
    stateBar->addWidget(m_retry);
    layout->addLayout(stateBar);
    m_model = new ImageEntryModel(this);
    m_proxy = new EntrySort(this);
    m_proxy->setSourceModel(m_model);
    m_view = new QTreeView(this);
    m_view->setObjectName(QStringLiteral("imageBrowserView"));
    m_view->setModel(m_proxy);
    m_view->setRootIsDecorated(false);
    m_view->setItemsExpandable(false);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(m_view, 1);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ImageBrowserWidget::selectionChanged);
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        if (!isReady()) return;
        const auto name = index.siblingAtColumn(0);
        const auto path = name.data(PathRole).toString();
        const auto type = name.data(TypeRole).toUInt();
        if (type == S_IFDIR) navigate(path);
        else if (type == S_IFREG) Q_EMIT fileActivated(path);
    });
    connect(m_up, &QToolButton::clicked, this, [this] { navigate(parentPath(m_directory)); });
    connect(m_back, &QToolButton::clicked, this, [this] {
        if (!m_history.isEmpty()) navigate(m_history.takeLast(), false);
    });
    connect(m_retry, &QToolButton::clicked, this, [this] {
        m_identity = identity();
        navigate(m_directory, false);
    });
    auto *upAction = new QAction(this);
    upAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Up));
    upAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addAction(upAction);
    connect(upAction, &QAction::triggered, m_up, &QToolButton::click);
    updateNavigation();
}

ImageBrowserWidget::~ImageBrowserWidget() { cancel(); }
void ImageBrowserWidget::setExtractAction(QAction *action) { m_extract->setDefaultAction(action); }
std::optional<dwarfsctl_kde::FileIdentity> ImageBrowserWidget::identity() const
{
    return dwarfsctl_kde::fileIdentity(m_image);
}
bool ImageBrowserWidget::isReady() const { return m_ready && m_identity && m_identity == identity(); }
QStringList ImageBrowserWidget::selectedPaths() const
{
    QStringList result;
    if (isReady()) {
        for (const auto &index : m_view->selectionModel()->selectedRows()) result.append(index.data(PathRole).toString());
    }
    return result;
}
void ImageBrowserWidget::cancel()
{
    m_readRetry->cancel();
    ++m_generation;
    if (m_job) {
        auto job = m_job;
        m_job = nullptr;
        job->kill(KJob::Quietly);
    }
}
void ImageBrowserWidget::setImage(const QString &path)
{
    const bool changed = path != m_image;
    if (!changed && m_identity == identity()) return;
    cancel();
    m_ready = false;
    m_image = path;
    m_identity = identity();
    if (changed) { m_directory = QStringLiteral("/"); m_history.clear(); }
    if (path.isEmpty()) {
        m_model->replace();
        m_path->setText(m_directory);
        m_state->setText(i18n("Select an image to browse its files."));
        m_retry->hide();
        updateNavigation();
        Q_EMIT selectionChanged();
        return;
    }
    navigate(m_directory, false);
}
void ImageBrowserWidget::updateNavigation()
{
    m_back->setEnabled(!m_image.isEmpty() && !m_history.isEmpty());
    m_up->setEnabled(!m_image.isEmpty() && m_directory != QLatin1String("/"));
}
void ImageBrowserWidget::navigate(const QString &path, bool remember)
{
    if (m_image.isEmpty()) return;
    cancel();
    m_ready = false;
    if (remember && path != m_directory) m_history.append(m_directory);
    m_directory = path;
    m_path->setText(path);
    m_model->replace();
    m_state->setText(i18n("Loading files…"));
    m_retry->hide();
    updateNavigation();
    Q_EMIT selectionChanged();
    request();
}
void ImageBrowserWidget::fail(const QString &message)
{
    m_ready = false;
    m_model->replace();
    m_state->setText(message);
    m_retry->show();
    Q_EMIT selectionChanged();
}
void ImageBrowserWidget::request()
{
    const auto generation = m_generation;
    m_job = dwarfsplugin::BackendQuery::ui(this,
        [this, generation](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (generation != m_generation) return;
        if (!ok) { m_job = nullptr; fail(error); return; }
        auto *job = new dwarfsplugin::DwarfsctlJob(
            {i18n("Read image files"), {QStringLiteral("ls"), m_image, QStringLiteral("--path"), m_directory,
            QStringLiteral("--children-only")}, config.command_timeout_ms, false}, this);
        m_job = job;
        connect(job, &KJob::result, this, [this, job, generation] {
            if (generation != m_generation || m_job != job) return;
            m_job = nullptr;
            if (m_identity != identity()) { fail(i18n("The image changed. Retry to refresh its files.")); return; }
            const auto envelope = job->envelope();
            if (job->error() || !envelope.ok) {
                if (m_readRetry->schedule(envelope, [this, generation] { if (generation == m_generation) request(); })) return;
                fail(envelope.error.message.isEmpty() ? job->errorString() : envelope.error.message);
                return;
            }
            m_readRetry->cancel();
            const auto value = envelope.details.value(QStringLiteral("entries"));
            if (!value.isArray()) { fail(i18n("The image listing is invalid.")); return; }
            const auto entries = value.toArray();
            QSet<QString> paths;
            paths.reserve(entries.size());
            QVector<ImageEntryModel::Entry> rows;
            rows.reserve(entries.size());
            for (const auto &value : entries) {
                const auto entry = value.toObject();
                const auto path = entry.value(QStringLiteral("path")).toString();
                const auto mode = entry.value(QStringLiteral("mode"));
                const auto size = entry.value(QStringLiteral("size"));
                const auto name = entry.value(QStringLiteral("name"));
                if (!value.isObject() || !validPath(path) || paths.contains(path)
                    || !name.isString() || !integer(mode, 4294967295.0)
                    || !(size.isNull() || integer(size, 9007199254740991.0))
                    || (path == m_directory || parentPath(path) != m_directory)
                    || (path != QLatin1String("/") && name.toString() != path.mid(path.lastIndexOf(QLatin1Char('/')) + 1))) {
                    fail(i18n("The image listing is invalid.")); return;
                }
                paths.insert(path);
                rows.push_back({name.toString(), path,
                    size.isNull() ? QStringLiteral("—") : QLocale().formattedDataSize(size.toInteger()),
                    mode.toVariant().toUInt() & S_IFMT,
                    size.isNull() ? -1 : size.toInteger()});
            }
            m_model->replace(std::move(rows));
            m_ready = true;
            m_state->setText(entries.isEmpty() ? i18n("This directory is empty.") : i18np("1 entry", "%1 entries", entries.size()));
            Q_EMIT selectionChanged();
        });
        job->start();
    });
}
