// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsfileitemactionplugin.h"
#include "dwarfsctlrunner.h"

#include <functional>

#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QUrl>
#include <QDesktopServices>
#include <QVariant>

#include <KFileItemListProperties>
#include <KJobWidgets>
#include <KPluginFactory>
#include <KLocalizedString>

#include "builddialog.h"
#include "buildfeedback.h"
#include <QPointer>
#include "traydbus.h"
#include "mountscache.h"
#include "dwarfsctljson.h"
#include "dwarfsctljob.h"
#include "trayagentjob.h"
#include "dwarfsctlrunner.h"
#include "logging.h"
#include "notifications.h"
#include "detailsdialog.h"
#include "integritycheckdialog.h"
#include "checksumdialog.h"

K_PLUGIN_FACTORY_WITH_JSON(DwarfsFileItemActionPluginFactory, DWARFSCTL_DOLPHIN_METADATA_FILE, registerPlugin<DwarfsFileItemActionPlugin>();)

DwarfsFileItemActionPlugin::DwarfsFileItemActionPlugin(QObject *parent)
    : KAbstractFileItemActionPlugin(parent)
{
    dwarfsplugin::MountsCache::instance()->ensureStarted();
}

namespace {


// Strict SSOT rule: tool schema/metadata must come from dwarfsctl at runtime.
// The plugin fails closed (no actions) if the schema cannot be obtained/parsed.
static bool ensureToolsSchemaAvailable(QWidget *parent)
{
    static int cached = 0; // 0=unknown, 1=ok, -1=bad
    if (cached == 1) {
        return true;
    }
    if (cached == -1) {
        return false;
    }

    using dwarfsplugin::parseEnvelope;

    const auto res = DwarfsctlRunner::run({QStringLiteral("tools"), QStringLiteral("schema")},
                                          DwarfsctlRunner::defaultTimeoutMs());

    if (res.crashed || res.exit_code != 0) {
        cached = -1;
        QMessageBox::critical(parent,
                              i18n("DwarFS integration unavailable"),
                              i18n("Unable to execute dwarfsctl tools schema.\n\n"
                                   "This integration is strict and will not guess tool metadata.\n"
                                   "Please install or fix dwarfsctl and ensure it is on PATH."));
        return false;
    }

    const auto pr = parseEnvelope(res.stdout_bytes);
    if (!pr.parsed || !pr.envelope.ok) {
        cached = -1;
        QMessageBox::critical(parent,
                              i18n("DwarFS integration unavailable"),
                              i18n("dwarfsctl returned an invalid tools schema payload.\n\n"
                                   "This integration is strict and will not fall back to hardcoded defaults."));
        return false;
    }
    const QJsonValue toolsV = pr.envelope.details.value(QStringLiteral("tools"));
    if (!toolsV.isArray()) {
        cached = -1;
        QMessageBox::critical(parent,
                              i18n("DwarFS integration unavailable"),
                              i18n("dwarfsctl tools schema is missing the required 'tools' array."));
        return false;
    }

    cached = 1;
    return true;
}

static void showErrorDialogWithDetails(QWidget *parent, const QString &title, const QString &msg, const dwarfsplugin::Envelope &env)
{
    qCWarning(DWARFS_DOLPHIN_LOG) << title << msg;
    dwarfsplugin::criticalModalWithDetails(parent, title, msg, env);
}

static QString localPathFromSingleUrl(const KFileItemListProperties &props)
{
    // Dolphin can pass multiple selected items. For actions that only support a
    // single target, callers still use the first selected URL.
    const auto urls = props.urlList();
    if (urls.isEmpty()) {
        return {};
    }
    const QUrl url = urls.first();
    if (!url.isLocalFile()) {
        return {};
    }
    return url.toLocalFile();
}

static bool allSelectedAreLocalAndHaveSuffix(const KFileItemListProperties &props, const QString &suffix)
{
    const auto urls = props.urlList();
    if (urls.isEmpty()) {
        return false;
    }
    for (const QUrl &u : urls) {
        if (!u.isLocalFile()) {
            return false;
        }
        if (!u.toLocalFile().endsWith(suffix, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

static QString envMountpoint(const dwarfsplugin::Envelope &env)
{
    const auto mp = env.details.value(QStringLiteral("mountpoint")).toString();
    if (!mp.isEmpty()) {
        return mp;
    }
    return {};
}

static void runDwarfsctlJob(QWidget *parentWidget,
                           const QString &title,
                           const QStringList &args,
                           int timeoutMs,
                           const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    using dwarfsplugin::DwarfsctlJob;

    qCInfo(DWARFS_DOLPHIN_LOG) << "Starting dwarfsctl" << title << "args=" << args;

    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{title, args, timeoutMs}, parentWidget);
    if (parentWidget) {
        KJobWidgets::setWindow(job, parentWidget);
    }

    QPointer<dwarfsplugin::BuildFeedback> feedback;
    if (args.value(0) == QLatin1String("build")) {
        const int outputIndex = args.indexOf(QStringLiteral("--output"));
        feedback = new dwarfsplugin::BuildFeedback(args.value(outputIndex + 1), parentWidget);
        QObject::connect(job, &DwarfsctlJob::buildPhase, feedback, &dwarfsplugin::BuildFeedback::phase);
    }
    QObject::connect(job, &KJob::result, job, [parentWidget, onSuccess, title, feedback](KJob *j) {
        auto *dj = qobject_cast<DwarfsctlJob *>(j);
        if (!dj) {
            return;
        }
        // mkdwarfs version warnings are surfaced in dialogs that have an inline banner
        // (e.g. the Build dialog). Jobs invoked from the context menu may not have a
        // suitable host UI; do not consume or display the warning here.
        const auto env = dj->envelope();

        if (dj->error() != 0) {
            if (feedback) feedback->failed();
            // Strict UI rule: failures are a critical modal.
            const QString msg = !env.error.message.isEmpty() ? env.error.message : dj->errorText();
            showErrorDialogWithDetails(parentWidget,
                                       title,
                                       msg.isEmpty() ? i18n("Operation failed") : msg,
                                       env);
            return;
        }

        const auto kind = dwarfsplugin::classifyEnvelope(env);
        if (kind == dwarfsplugin::NotifyKind::Warning) {
            dwarfsplugin::notifyResult(parentWidget,
                                       kind,
                                       title,
                                       i18n("Completed with warnings"));
        }

        if (feedback) feedback->complete(env);
        onSuccess(env);
    });

    job->start();
}

static void runTrayAgentJob(QWidget *parentWidget,
                            const QString &title,
                            const QString &method,
                            const QList<QVariant> &args,
                            int timeoutMs,
                            const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    using dwarfsplugin::TrayAgentJob;

    qCInfo(DWARFS_DOLPHIN_LOG) << "Starting tray D-Bus job" << title << "method=" << method;

    auto *job = new TrayAgentJob(TrayAgentJob::Request{title, method, args, timeoutMs}, parentWidget);
    if (parentWidget) {
        KJobWidgets::setWindow(job, parentWidget);
    }

    QObject::connect(job, &KJob::result, parentWidget, [parentWidget, onSuccess, title](KJob *j) {
        auto *tj = qobject_cast<TrayAgentJob *>(j);
        if (!tj) {
            return;
        }
        const auto env = tj->envelope();

        if (tj->error() != 0) {
            const QString msg = !env.error.message.isEmpty() ? env.error.message : tj->errorText();
            showErrorDialogWithDetails(parentWidget,
                                       title,
                                       msg.isEmpty() ? i18n("Operation failed") : msg,
                                       env);
            return;
        }

        const auto kind = dwarfsplugin::classifyEnvelope(env);
        if (kind == dwarfsplugin::NotifyKind::Warning) {
            dwarfsplugin::notifyResult(parentWidget,
                                       kind,
                                       title,
                                       i18n("Completed with warnings"));
        }
        onSuccess(env);
    });

    job->start();
}

} // namespace

QList<QAction *> DwarfsFileItemActionPlugin::actions(const KFileItemListProperties &fileItemInfos, QWidget *parentWidget)
{
    const QString path = localPathFromSingleUrl(fileItemInfos);
    if (path.isEmpty()) {
        return {};
    }

    // Fail-closed if dwarfsctl tool schema is not available.
    if (!ensureToolsSchemaAvailable(parentWidget)) {
        return {};
    }

    // The plugin can be invoked for generic file types (e.g. before the
    // application's mime database is refreshed). Only show file actions for
    // *.dwarfs files.
    if (!fileItemInfos.isDirectory() && !allSelectedAreLocalAndHaveSuffix(fileItemInfos, QStringLiteral(".dwarfs"))) {
        return {};
    }

    // Menus are constructed per-item type below.

    if (fileItemInfos.isDirectory()) {
        if (fileItemInfos.urlList().size() != 1) {
            // Building or unmounting requires one directory.
            return {};
        }

        // A mounted DwarFS volume is a directory as far as Dolphin is
        // concerned. Recognise it before offering the generic "Build image"
        // action so the mounted location itself has an obvious eject control.
        auto *mounts = dwarfsplugin::MountsCache::instance();
        mounts->ensureStarted();
        const QString mountedImage = mounts->imageForMountpoint(path);
        if (!mountedImage.isEmpty()) {
            auto *unmount = new QAction(QIcon::fromTheme(QStringLiteral("media-eject")),
                                        i18n("Unmount DwarFS Volume"),
                                        parentWidget);
            unmount->setIconVisibleInMenu(true);
            QObject::connect(unmount, &QAction::triggered, parentWidget,
                             [parentWidget, mountedImage, mountpoint = path]() {
                dwarfsplugin::ensureTrayRunning();
                runTrayAgentJob(parentWidget,
                                i18n("Unmount %1", QFileInfo(mountedImage).fileName()),
                                QStringLiteral("UnmountByMountpoint"),
                                {mountpoint},
                                DwarfsctlRunner::defaultTimeoutMs(),
                                [](const dwarfsplugin::Envelope &) {});
            });
            return {unmount};
        }

        // Folder context menu:
        //   Build image -> DwarFS
        auto *rootMenu = new QMenu(parentWidget);
        rootMenu->setTitle(i18n("Build image"));

        auto *rootAction = new QAction(QIcon::fromTheme(QStringLiteral("archive-new")), i18n("Build image"), parentWidget);
        rootAction->setMenu(rootMenu);
        rootAction->setIconVisibleInMenu(true);

        QAction *buildAction = new QAction(QIcon::fromTheme(QStringLiteral("document-save")), i18n("DwarFS"), rootMenu);
        buildAction->setIconVisibleInMenu(true);
        QObject::connect(buildAction, &QAction::triggered, parentWidget, [parentWidget, path]() {
            const auto res = dwarfsplugin::promptBuildOptions(parentWidget, path);
            if (!res.accepted) {
                return;
            }

            if (res.outputPath.isEmpty()) {
                QMessageBox::critical(parentWidget, i18n("Build DwarFS image"), i18n("Output path is required."));
                return;
            }

            QStringList args;
            args << QStringLiteral("build")
                 << QStringLiteral("--output") << res.outputPath;
            for (const QString &a : res.profileArgs) {
                // Avoid emitting a bare "--mkdwarfs-arg" with an empty value; this
                // produces a Clap parse error that looks like a plugin bug (it is).
                const QString t = a.trimmed();
                if (t.isEmpty()) {
                    continue;
                }
                args << QStringLiteral("--mkdwarfs-arg") << t;
            }
            if (res.force) {
                args << QStringLiteral("--force");
            }
            if (res.overwriteOutput) {
                args << QStringLiteral("--overwrite-output");
            }
            if (res.yes) {
                args << QStringLiteral("--yes");
            }
            if (res.dryRun) {
                args << QStringLiteral("--dry-run");
            }
            // SOURCE_DIR is a positional argument; keep it last so Clap doesn't
            // treat later options as additional positionals.
            args << path;

            runDwarfsctlJob(parentWidget,
                           i18n("Build DwarFS image"),
                           args,
                           DwarfsctlRunner::buildTimeoutMs(),
                           [](const dwarfsplugin::Envelope &env) {
                               const QString out = env.image.isEmpty() ? i18n("(unknown)") : env.image;
                              qCInfo(DWARFS_DOLPHIN_LOG) << "Build completed successfully output=" << out;
                           });
        });

        rootMenu->addAction(buildAction);
        return {rootAction};
    }

    // File context menu (.dwarfs): DwarFS -> actions
    auto *menu = new QMenu(parentWidget);
    menu->setTitle(i18n("DwarFS"));

    auto *rootAction = new QAction(QIcon::fromTheme(QStringLiteral("dwarfsctlimage")), i18n("DwarFS"), parentWidget);
    rootAction->setMenu(menu);
    rootAction->setIconVisibleInMenu(true);

    const QString imagePath = path;
    const QString imageName = QFileInfo(imagePath).fileName();

    // Menu construction must stay instant. MountsCache is updated by the tray's
    // mount-change signal, so no blocking D-Bus call is needed while Dolphin is
    // opening a context menu.
    auto *mounts = dwarfsplugin::MountsCache::instance();
    mounts->ensureStarted();
    const QString currentMountpoint = mounts->mountpointForImage(imagePath);
    const bool mounted = !currentMountpoint.isEmpty();

    QAction *openAction = new QAction(QIcon::fromTheme(QStringLiteral("document-open-folder")), i18n("Open Mounted Volume"), menu);
    openAction->setIconVisibleInMenu(true);
    QObject::connect(openAction, &QAction::triggered, parentWidget, [mountpoint = currentMountpoint]() {
        if (!mountpoint.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
        }
    });

    // Extract
    QAction *extractAction = new QAction(QIcon::fromTheme(QStringLiteral("archive-extract")), i18n("Extract…"), menu);
    extractAction->setIconVisibleInMenu(true);
    QObject::connect(extractAction, &QAction::triggered, parentWidget, [parentWidget, imagePath, imageName]() {
        const QString parentDir = QFileDialog::getExistingDirectory(
            parentWidget, i18n("Choose a Folder for the Extracted Image"), QFileInfo(imagePath).absolutePath());
        if (parentDir.isEmpty()) {
            return;
        }
        QString folderName = QFileInfo(imageName).completeBaseName();
        if (folderName.isEmpty()) {
            folderName = i18n("Extracted DwarFS Image");
        }
        QDir outputParent(parentDir);
        QString dest = outputParent.filePath(folderName);
        for (int suffix = 2;; ++suffix) {
            const QFileInfo candidate(dest);
            if (!candidate.exists() && !candidate.isSymLink()) {
                break;
            }
            dest = outputParent.filePath(i18n("%1 (%2)", folderName, suffix));
        }
        QStringList args;
        args << QStringLiteral("extract") << imagePath << dest;
        runDwarfsctlJob(parentWidget,
                        i18n("Extract %1", imageName),
                        args,
                        DwarfsctlRunner::buildTimeoutMs(),
                        [parentWidget, imageName, dest](const dwarfsplugin::Envelope &) {
                            QDesktopServices::openUrl(QUrl::fromLocalFile(dest));
                            dwarfsplugin::notifyResult(parentWidget,
                                                       dwarfsplugin::NotifyKind::Info,
                                                       i18n("Extraction Complete"),
                                                       i18n("%1 was extracted to %2.",
                                                            imageName,
                                                            QDir::toNativeSeparators(dest)));
                        });
    });

    QAction *checkAction = new QAction(QIcon::fromTheme(QStringLiteral("tools-check-spelling")), i18n("Check Integrity…"), menu);
    checkAction->setIconVisibleInMenu(true);
    QObject::connect(checkAction, &QAction::triggered, parentWidget, [parentWidget, imagePath]() {
        dwarfsplugin::IntegrityCheckDialog dialog(imagePath, parentWidget);
        dialog.exec();
    });

    QAction *checksumAction = new QAction(QIcon::fromTheme(QStringLiteral("document-encrypt")), i18n("Checksums…"), menu);
    checksumAction->setIconVisibleInMenu(true);
    QObject::connect(checksumAction, &QAction::triggered, parentWidget, [parentWidget, imagePath]() {
        dwarfsplugin::ChecksumDialog dialog(imagePath, parentWidget);
        dialog.exec();
    });

    // Mount
    QAction *mountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-mount")), i18n("Mount and Open"), menu);
    mountAction->setIconVisibleInMenu(true);
    QObject::connect(mountAction, &QAction::triggered, parentWidget, [parentWidget, imagePath, imageName]() {
        dwarfsplugin::ensureTrayRunning();
        runTrayAgentJob(parentWidget,
                       i18n("Mount and Open %1", imageName),
                       QStringLiteral("Mount"),
                       {imagePath},
                       DwarfsctlRunner::defaultTimeoutMs(),
                       [parentWidget](const dwarfsplugin::Envelope &env) {
                           const QString mountpoint = envMountpoint(env);
                           // Mounting an image is how you look inside it, so open it
                           // unless the user has turned that off. This is the behaviour
                           // that makes a .dwarfs file feel like a disk image rather
                           // than a file you have to go find afterwards.
                           if (!DwarfsctlRunner::openAfterMount() || mountpoint.isEmpty()) {
                               return;
                           }
                           QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
                       });
    });

    // Unmount
    QAction *unmountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-eject")), i18n("Unmount"), menu);
    unmountAction->setIconVisibleInMenu(true);
    QObject::connect(unmountAction, &QAction::triggered, parentWidget,
                     [parentWidget, imagePath, imageName, currentMountpoint]() {
        dwarfsplugin::ensureTrayRunning();
        const bool haveMountpoint = !currentMountpoint.isEmpty();
        const QString method = haveMountpoint ? QStringLiteral("UnmountByMountpoint")
                                              : QStringLiteral("UnmountByImage");
        const QList<QVariant> args = haveMountpoint ? QList<QVariant>{currentMountpoint}
                                                    : QList<QVariant>{imagePath};

        runTrayAgentJob(parentWidget,
                       i18n("Unmount %1", imageName),
                       method,
                       args,
                       DwarfsctlRunner::defaultTimeoutMs(),
                       [](const dwarfsplugin::Envelope &) {});
    });

    // Put the task the user came for first, and keep diagnostic tools later.
    if (mounted) {
        menu->addAction(openAction);
        menu->addAction(unmountAction);
    } else {
        menu->addAction(mountAction);
    }

    menu->addAction(extractAction);
    menu->addSeparator();
    menu->addAction(checkAction);
    menu->addAction(checksumAction);

    return {rootAction};
}

#include "dwarfsfileitemactionplugin.moc"
