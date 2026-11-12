// SPDX-License-Identifier: GPL-3.0-or-later

#include "statusadvanceddialog.h"

#include <KFormat>
#include <KLocalizedString>
#include <KPageWidgetItem>

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace dwarfsplugin {
namespace {

QLabel *valueLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

void addRow(QFormLayout *form, const QString &label, const QString &value, QWidget *parent)
{
    if (!value.trimmed().isEmpty())
        form->addRow(label, valueLabel(value, parent));
}

QGroupBox *formGroup(const QString &title,
                     const QList<QPair<QString, QString>> &rows,
                     QWidget *parent)
{
    auto *group = new QGroupBox(title, parent);
    auto *form = new QFormLayout(group);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    for (const auto &row : rows)
        addRow(form, row.first, row.second, group);
    return group;
}

QWidget *scrollPage(const QString &description, QWidget *parent, QVBoxLayout **contentOut)
{
    auto *content = new QWidget(parent);
    auto *layout = new QVBoxLayout(content);
    auto *intro = new QLabel(description, content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    *contentOut = layout;
    return scroll;
}

QString count(qint64 value)
{
    return value >= 0 ? QLocale().toString(value) : QString();
}

QString bytes(qint64 value)
{
    return value >= 0 ? KFormat().formatByteSize(value) : QString();
}

} // namespace

StatusAdvancedDialog::StatusAdvancedDialog(const Envelope &env,
                                           const StatusDetails &details,
                                           QWidget *parent)
    : KPageDialog(parent)
{
    setWindowTitle(i18n("Technical Details"));
    setFaceType(KPageDialog::List);
    setStandardButtons(QDialogButtonBox::Close);
    setMinimumSize(780, 540);

    {
        QVBoxLayout *layout = nullptr;
        auto *page = scrollPage(
            i18n("Diagnostic state reported by dwarfsctl for this image."),
            this, &layout);
        layout->addWidget(formGroup(i18n("Backend Request"), {
            {i18n("Image:"), env.image},
            {i18n("Operation:"), env.command},
            {i18n("Result:"), env.ok ? i18n("Successful") : i18n("Failed")},
        }, page));
        layout->addWidget(formGroup(i18n("Mounted Volume"), {
            {i18n("Mount point:"), details.mountpoint},
            {i18n("Health:"), details.mount_health},
            {i18n("Filesystem:"), details.mount_fs_type},
            {i18n("Source:"), details.mount_source},
        }, page));
        layout->addStretch(1);
        auto *item = addPage(page, i18n("Overview"));
        item->setIcon(QIcon::fromTheme(QStringLiteral("dialog-information")));
    }

    {
        QVBoxLayout *layout = nullptr;
        auto *page = scrollPage(
            i18n("Low-level filesystem organization and stable image identity."),
            this, &layout);
        layout->addWidget(formGroup(i18n("Filesystem Layout"), {
            {i18n("Inodes:"), count(details.inode_count)},
            {i18n("Blocks:"), count(details.block_count)},
            {i18n("Block size:"), bytes(details.block_size)},
            {i18n("Features:"), details.features.isEmpty() ? i18n("None") : details.features.join(QStringLiteral(", "))},
        }, page));
        layout->addWidget(formGroup(i18n("Identity"), {
            {i18n("Image hash:"), details.image_hash},
        }, page));
        layout->addStretch(1);
        auto *item = addPage(page, i18n("Storage"));
        item->setIcon(QIcon::fromTheme(QStringLiteral("drive-harddisk")));
    }

    {
        QVBoxLayout *layout = nullptr;
        auto *page = scrollPage(
            i18n("Resolved paths, coordination files and packaged engine components."),
            this, &layout);
        layout->addWidget(formGroup(i18n("Paths and Coordination"), {
            {i18n("Canonical image:"), details.canonical_image},
            {i18n("Expected mount point:"), details.derived_mountpoint},
            {i18n("Lock file:"), details.lock_path},
        }, page));

        auto *tools = new QGroupBox(i18n("Engine Components"), page);
        auto *toolsLayout = new QVBoxLayout(tools);
        auto *tree = new QTreeWidget(tools);
        tree->setRootIsDecorated(false);
        tree->setAlternatingRowColors(true);
        tree->setSelectionMode(QAbstractItemView::NoSelection);
        tree->setHeaderLabels({i18n("Component"), i18n("Status"), i18n("Location")});
        for (auto it = details.deps.cbegin(); it != details.deps.cend(); ++it) {
            const auto &info = it.value();
            QString status = info.found ? i18n("Available") : i18n("Unavailable");
            if (!info.installed_version.isEmpty())
                status += i18n(" · %1", info.installed_version);
            new QTreeWidgetItem(tree, {it.key(), status, info.path});
        }
        if (details.deps.isEmpty())
            new QTreeWidgetItem(tree, {i18n("Packaged engine"), i18n("No component report"), QString()});
        tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
        tree->setMinimumHeight(150);
        toolsLayout->addWidget(tree);
        layout->addWidget(tools);
        layout->addStretch(1);
        auto *item = addPage(page, i18n("Runtime"));
        item->setIcon(QIcon::fromTheme(QStringLiteral("system-run")));
    }

    {
        auto *page = new QWidget(this);
        auto *layout = new QVBoxLayout(page);
        auto *intro = new QLabel(
            i18n("Complete machine-readable report for troubleshooting and bug reports."), page);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        m_jsonEdit = new QPlainTextEdit(page);
        m_jsonEdit->setReadOnly(true);
        m_jsonEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
        m_jsonEdit->setPlainText(prettyJsonForEnvelope(env));
        layout->addWidget(m_jsonEdit, 1);
        auto *item = addPage(page, i18n("Report"));
        item->setIcon(QIcon::fromTheme(QStringLiteral("text-x-json")));
    }

    auto *copyButton = buttonBox()->addButton(i18n("Copy Report"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_jsonEdit->toPlainText());
    });
}

QString StatusAdvancedDialog::prettyJsonForEnvelope(const Envelope &env)
{
    QJsonObject root;
    root.insert(QStringLiteral("ok"), env.ok);
    if (!env.command.isEmpty())
        root.insert(QStringLiteral("command"), env.command);
    if (!env.image.isEmpty())
        root.insert(QStringLiteral("image"), env.image);
    if (!env.details.isEmpty())
        root.insert(QStringLiteral("details"), env.details);
    if (env.has_error) {
        QJsonObject error;
        if (env.error.code != 0)
            error.insert(QStringLiteral("code"), env.error.code);
        if (!env.error.kind.isEmpty())
            error.insert(QStringLiteral("kind"), env.error.kind);
        if (!env.error.message.isEmpty())
            error.insert(QStringLiteral("message"), env.error.message);
        root.insert(QStringLiteral("error"), error);
    }
    if (!env.warnings.isEmpty()) {
        QJsonArray warnings;
        for (const auto &warning : env.warnings)
            warnings.append(warning);
        root.insert(QStringLiteral("warnings"), warnings);
    }
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

} // namespace dwarfsplugin
