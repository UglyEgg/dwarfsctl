// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfspropertiesplugin.h"

#include "dwarfsctljob.h"
#include "dwarfsctljson.h"
#include "dwarfsctlmodel.h"
#include "dwarfsctlrunner.h"
#include "detailsdialog.h"
#include "notifications.h"
#include "statusadvanceddialog.h"
#include "traydbus.h"
#include "mountscache.h"
#include "trayagentjob.h"

#include <KJobWidgets>
#include <KLocalizedString>
#include <KPluginFactory>
#include <KPropertiesDialog>


#include <QAction>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>
#include <QtMath>

#include <memory>

namespace dwarfsplugin {

struct View {
    QLabel *contents = nullptr;
    QLabel *originalSize = nullptr;
    QLabel *compression = nullptr;
    QLabel *created = nullptr;
    QLabel *createdBy = nullptr;
    QLabel *formatVersion = nullptr;
    QLabel *mounted = nullptr;
    QLabel *mountpoint = nullptr;
    QString currentMountpoint;
    Envelope statusEnvelope;
    StatusDetails statusDetails;
    bool hasTechnicalDetails = false;

    QPushButton *primaryButton = nullptr;
    QPushButton *technicalButton = nullptr;
};

static StatusDetails cachedMountStatus(const QString &imagePath)
{
    auto *mounts = MountsCache::instance();
    mounts->ensureStarted();
    StatusDetails details;
    details.mounted_present = true;
    details.mountpoint = mounts->mountpointForImage(imagePath);
    details.mounted = !details.mountpoint.isEmpty();
    return details;
}


static void refreshUi(const StatusDetails &det, View &v)
{
    const bool mounted = det.mounted_present && det.mounted;
    v.currentMountpoint = mounted ? det.mountpoint : QString();

    if (det.mounted_present) {
        v.mounted->setText(det.mounted ? i18n("Mounted") : i18n("Not mounted"));
    } else {
        v.mounted->setText(i18n("Unknown"));
    }

    v.mountpoint->setText(v.currentMountpoint.isEmpty() ? i18n("(none)") : v.currentMountpoint);

    v.primaryButton->setText(mounted ? i18n("Open Volume") : i18n("Mount and Open"));
    v.primaryButton->setIcon(QIcon::fromTheme(mounted ? QStringLiteral("document-open-folder")
                                                      : QStringLiteral("media-mount")));
    v.primaryButton->setEnabled(!mounted || !v.currentMountpoint.isEmpty());
}

static void refreshImageMetadata(const QString &imagePath, const StatusDetails &det, View &v)
{
    const QFileInfo fi(imagePath);
    const QLocale locale;

    if (!det.image_metadata_present) {
        for (QLabel *label : {v.contents, v.originalSize, v.compression, v.created, v.createdBy, v.formatVersion}) {
            label->setText(i18n("(unavailable)"));
        }
        v.technicalButton->setEnabled(false);
        return;
    }

    v.contents->setText(det.file_count >= 0 && det.directory_count >= 0
                            ? i18n("%1 files, %2 folders", det.file_count, det.directory_count)
                            : i18n("(unavailable)"));
    v.originalSize->setText(det.original_size >= 0
                                ? locale.formattedDataSize(det.original_size)
                                : i18n("(unavailable)"));
    if (det.original_size > 0 && fi.size() > 0 && det.original_size > fi.size()) {
        const double ratio = static_cast<double>(det.original_size) / static_cast<double>(fi.size());
        const int saved = qRound(100.0 * (1.0 - static_cast<double>(fi.size()) / static_cast<double>(det.original_size)));
        v.compression->setText(i18n("%1× smaller (%2% saved)", locale.toString(ratio, 'f', 1), saved));
    } else if (det.original_size >= 0 && fi.size() > 0) {
        v.compression->setText(i18n("No size reduction"));
    } else {
        v.compression->setText(i18n("(unavailable)"));
    }
    v.created->setText(det.created_on.isEmpty() ? i18n("(unavailable)") : det.created_on);
    v.createdBy->setText(det.created_by.isEmpty() ? i18n("(unavailable)") : det.created_by);
    v.formatVersion->setText(det.format_version.isEmpty() ? i18n("(unavailable)") : det.format_version);
    v.technicalButton->setEnabled(v.hasTechnicalDetails);
}

static void applyLiveMountStatus(const StatusDetails &mount, View &v)
{
    if (v.hasTechnicalDetails) {
        v.statusDetails.mounted_present = mount.mounted_present;
        v.statusDetails.mounted = mount.mounted;
        v.statusDetails.mountpoint = mount.mounted ? mount.mountpoint : QString();
        v.statusDetails.mount_health = mount.mount_health;
        v.statusEnvelope.details.insert(QStringLiteral("mounted"), mount.mounted);
        v.statusEnvelope.details.insert(QStringLiteral("mountpoint"),
                                        mount.mounted && !mount.mountpoint.isEmpty()
                                            ? QJsonValue(mount.mountpoint)
                                            : QJsonValue(QJsonValue::Null));
        if (!mount.mount_health.isEmpty()) {
            v.statusEnvelope.details.insert(QStringLiteral("mount_health"), mount.mount_health);
        } else {
            v.statusEnvelope.details.remove(QStringLiteral("mount_health"));
        }
    }
    refreshUi(mount, v);
}

static void runJob(QWidget *parent,
                   const QString &title,
                   const QStringList &args,
                   int timeoutMs,
                   const std::function<void(const Envelope &)> &onDone)
{
    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{title, args, timeoutMs}, parent);
    if (parent) {
        KJobWidgets::setWindow(job, parent);
    }

    QObject::connect(job, &KJob::result, parent, [parent, title, onDone](KJob *j) {
        auto *dj = qobject_cast<DwarfsctlJob *>(j);
        if (!dj) {
            return;
        }
        const auto env = dj->envelope();
        if (dj->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : dj->errorText();
            criticalModalWithDetails(parent, title, msg.isEmpty() ? i18n("Operation failed") : msg, env);
            return;
        }
        if (classifyEnvelope(env) == NotifyKind::Warning) {
            notifyResult(parent, NotifyKind::Warning, title, i18n("Completed with warnings"));
        }
        onDone(env);
    });

    job->start();
}

static void runTrayAgentJob(QWidget *parent,
                            const QString &title,
                            const QString &method,
                            const QList<QVariant> &args,
                            int timeoutMs,
                            const std::function<void(const Envelope &)> &onDone)
{
    dwarfsplugin::ensureTrayRunning();

    auto *job = new TrayAgentJob(TrayAgentJob::Request{title, method, args, timeoutMs}, parent);
    if (parent) {
        KJobWidgets::setWindow(job, parent);
    }

    QObject::connect(job, &KJob::result, parent, [parent, title, onDone](KJob *j) {
        auto *tj = qobject_cast<TrayAgentJob *>(j);
        if (!tj) {
            return;
        }
        const auto env = tj->envelope();
        if (tj->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : tj->errorText();
            criticalModalWithDetails(parent, title, msg.isEmpty() ? i18n("Operation failed") : msg, env);
            return;
        }
        if (classifyEnvelope(env) == NotifyKind::Warning) {
            notifyResult(parent, NotifyKind::Warning, title, i18n("Completed with warnings"));
        }
        onDone(env);
    });

    job->start();
}

DwarfsPropertiesPlugin::DwarfsPropertiesPlugin(QObject *parent, const QVariantList &args)
    : KPropertiesDialogPlugin(parent)
{
    Q_UNUSED(args);

    auto *dialog = qobject_cast<KPropertiesDialog *>(parent);
    if (!dialog) {
        return;
    }

    const auto items = dialog->items();
    if (items.isEmpty()) {
        return;
    }
    const auto url = items.first().url();
    if (!url.isLocalFile()) {
        return;
    }
    const QString imagePath = url.toLocalFile();
    if (!imagePath.endsWith(QStringLiteral(".dwarfs"), Qt::CaseInsensitive)) {
        return;
    }

    auto *page = new QWidget(dialog);
    page->setObjectName(QStringLiteral("dwarfsImagePropertiesPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(14);

    // Connections outlive this constructor. Keep their state alive with the
    // page instead of capturing stack locals by reference.
    auto v = std::make_shared<View>();
    auto *detailsBox = new QGroupBox(i18n("Image details"), page);
    auto *form = new QFormLayout(detailsBox);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    v->contents = new QLabel(i18n("Reading…"), detailsBox);
    v->originalSize = new QLabel(i18n("Reading…"), detailsBox);
    v->compression = new QLabel(i18n("Reading…"), detailsBox);
    v->created = new QLabel(i18n("Reading…"), detailsBox);
    v->createdBy = new QLabel(i18n("Reading…"), detailsBox);
    v->formatVersion = new QLabel(i18n("Reading…"), detailsBox);
    v->createdBy->setWordWrap(true);

    form->addRow(i18n("Contents:"), v->contents);
    form->addRow(i18n("Original size:"), v->originalSize);
    form->addRow(i18n("Space saved:"), v->compression);
    form->addRow(i18n("Image created:"), v->created);
    form->addRow(i18n("Created by:"), v->createdBy);
    form->addRow(i18n("Format:"), v->formatVersion);
    layout->addWidget(detailsBox);

    auto *mountBox = new QGroupBox(i18n("Mounted volume"), page);
    auto *mountForm = new QFormLayout(mountBox);
    mountForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    mountForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    v->mounted = new QLabel(mountBox);
    v->mountpoint = new QLabel(mountBox);
    v->mountpoint->setWordWrap(true);
    v->mountpoint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mountForm->addRow(i18n("Status:"), v->mounted);
    mountForm->addRow(i18n("Location:"), v->mountpoint);
    layout->addWidget(mountBox);

    auto *buttonRow = new QHBoxLayout();
    v->primaryButton = new QPushButton(page);
    buttonRow->addWidget(v->primaryButton);
    buttonRow->addStretch(1);
    v->technicalButton = new QPushButton(QIcon::fromTheme(QStringLiteral("dialog-information")),
                                         i18n("Technical Details…"), page);
    v->technicalButton->setEnabled(false);
    buttonRow->addWidget(v->technicalButton);
    layout->addLayout(buttonRow);
    layout->addStretch(1);

    // Initial status
    auto *mounts = MountsCache::instance();
    mounts->ensureStarted();
    refreshUi(cachedMountStatus(imagePath), *v);
    QObject::connect(mounts, &MountsCache::mountsChanged, page, [imagePath, v]() {
        applyLiveMountStatus(cachedMountStatus(imagePath), *v);
    });

    QObject::connect(v->primaryButton, &QPushButton::clicked, page, [dialog, imagePath, v]() {
        if (!v->currentMountpoint.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(v->currentMountpoint));
            return;
        }

        runTrayAgentJob(dialog,
                        i18n("Mount and Open"),
                        QStringLiteral("Mount"),
                        {imagePath},
                        DwarfsctlRunner::defaultTimeoutMs(),
        [v](const Envelope &env) {
            const QString mountpoint = env.details.value(QStringLiteral("mountpoint")).toString();
            if (!mountpoint.isEmpty()) {
                StatusDetails mounted;
                mounted.mounted_present = true;
                mounted.mounted = true;
                mounted.mountpoint = mountpoint;
                applyLiveMountStatus(mounted, *v);
                QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
            }
        });
    });

    QObject::connect(v->technicalButton, &QPushButton::clicked, page, [dialog, v]() {
        if (!v->hasTechnicalDetails) {
            return;
        }
        StatusAdvancedDialog details(v->statusEnvelope, v->statusDetails, dialog);
        details.exec();
    });

    dialog->addPage(page, i18n("DwarFS Image"));

    // Reading the full DwarFS metadata can take noticeable time for a large
    // image. Populate the page asynchronously and keep mount controls usable
    // while it is loading.
    runJob(dialog,
           i18n("Read DwarFS Metadata"),
           {QStringLiteral("status"), imagePath},
           DwarfsctlRunner::defaultTimeoutMs(),
           [imagePath, v](const Envelope &richEnvelope) {
        Envelope combined = richEnvelope;

        // The tray remains the authority for live mount state. Overlay its
        // lightweight answer without discarding the backend's image metadata.
        const StatusDetails mountDetails = cachedMountStatus(imagePath);
        combined.details.insert(QStringLiteral("mounted"), mountDetails.mounted);
        combined.details.insert(QStringLiteral("mountpoint"),
                                mountDetails.mounted && !mountDetails.mountpoint.isEmpty()
                                    ? QJsonValue(mountDetails.mountpoint)
                                    : QJsonValue(QJsonValue::Null));

        StatusDetails details;
        if (!decodeStatus(combined, &details).ok) {
            return;
        }
        v->statusEnvelope = combined;
        v->statusDetails = details;
        v->hasTechnicalDetails = details.image_metadata_present;
        v->technicalButton->setEnabled(v->hasTechnicalDetails);
        refreshUi(details, *v);
        refreshImageMetadata(imagePath, details, *v);
    });
}

} // namespace dwarfsplugin

K_PLUGIN_FACTORY_WITH_JSON(DwarfsPropertiesPluginFactory, "dwarfspropertiesplugin.json", registerPlugin<dwarfsplugin::DwarfsPropertiesPlugin>();)

#include "dwarfspropertiesplugin.moc"
