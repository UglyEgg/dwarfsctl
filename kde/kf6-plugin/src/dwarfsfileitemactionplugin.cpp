// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsfileitemactionplugin.h"
#include "dwarfsctlrunner.h"

#include <functional>

#include <QAction>
#include <QApplication>
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
#include "extractionfeedback.h"
#include <QPointer>
#include <QProgressDialog>
#include "traydbus.h"
#include "mountscache.h"
#include "dwarfsctljson.h"
#include "dwarfsctljob.h"
#include "backendquery.h"
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


class MenuReadiness final : public QObject {
public:
    MenuReadiness(QObject *owner, const QList<QAction *> &actions,
                  QAction *status, QAction *retry)
        : QObject(owner), m_status(status), m_retry(retry)
    {
        for (auto *action : actions) m_actions.append(action);
        connect(retry, &QAction::triggered, this, [this] { refresh(); });
        refresh();
    }
private:
    void refresh()
    {
        if (m_job || !m_status || !m_retry) return;
        for (const auto &action : m_actions) if (action) action->setEnabled(false);
        m_status->setText(i18n("Preparing DwarFS actions…"));
        m_status->setVisible(true);
        m_retry->setVisible(false);
        m_job = dwarfsplugin::BackendQuery::json(this,
            {QStringLiteral("tools"), QStringLiteral("schema")},
            [this](const dwarfsplugin::BackendReply &reply) {
            m_job = nullptr;
            if (!m_status || !m_retry) return;
            const bool valid = reply.ok && reply.envelope.details.value(QStringLiteral("tools")).isArray();
            for (const auto &action : m_actions) if (action) action->setEnabled(valid);
            m_status->setText(i18n("DwarFS actions unavailable"));
            m_status->setToolTip(reply.ok ? i18n("The DwarFS tool information is invalid.") : reply.error);
            m_status->setVisible(!valid);
            m_retry->setVisible(!valid);
        });
    }
    QList<QPointer<QAction>> m_actions;
    QPointer<QAction> m_status;
    QPointer<QAction> m_retry;
    QPointer<dwarfsplugin::DwarfsctlJob> m_job;
};

static void prepareMenu(QMenu *menu)
{
    const auto actions = menu->actions();
    auto *status = menu->addAction(i18n("Preparing DwarFS actions…"));
    status->setEnabled(false);
    auto *retry = menu->addAction(i18n("Retry"));
    new MenuReadiness(menu, actions, status, retry);
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

static void runResolvedDwarfsctlJob(QWidget *parentWidget,
                           const QString &title,
                           const QStringList &args,
                           int timeoutMs,
                           const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    using dwarfsplugin::DwarfsctlJob;

    qCInfo(DWARFS_DOLPHIN_LOG) << "Starting dwarfsctl" << title << "args=" << args;

    QStringList jobArgs = args;
    QPointer<dwarfsplugin::BuildFeedback> feedback;
    QPointer<ExtractionFeedback> extraction;
    if (args.value(0) == QLatin1String("build")) {
        const int outputIndex = args.indexOf(QStringLiteral("--output"));
        const QString output = args.value(outputIndex + 1);
        const bool preview = args.contains(QStringLiteral("--preview"));
        feedback = new dwarfsplugin::BuildFeedback(output, parentWidget, preview);
        if (preview) {
            if (feedback->previewStage().isEmpty()) {
                feedback->failed(i18n("Could not prepare a private location for the measured image."));
                return;
            }
            jobArgs.insert(jobArgs.size() - 1, QStringLiteral("--preview-stage"));
            jobArgs.insert(jobArgs.size() - 1, feedback->previewStage());
            QObject::connect(feedback, &dwarfsplugin::BuildFeedback::createRequested, feedback,
                             [feedback, output, source = args.last(), force = args.contains(QStringLiteral("--force")),
                              parentWidget, timeoutMs, onSuccess](const QString &candidate, qint64 size) {
                QStringList publishArgs{QStringLiteral("build"), QStringLiteral("--output"), output,
                                        QStringLiteral("--publish-preview"), candidate,
                                        QStringLiteral("--expected-size"), QString::number(size)};
                if (force) publishArgs << QStringLiteral("--force");
                publishArgs << source;
                auto *publishJob = new DwarfsctlJob(
                    DwarfsctlJob::Request{i18n("Create DwarFS Image"), publishArgs, timeoutMs, false}, feedback);
                if (parentWidget) KJobWidgets::setWindow(publishJob, parentWidget);
                QObject::connect(publishJob, &DwarfsctlJob::buildPhase, feedback,
                                 &dwarfsplugin::BuildFeedback::phase);
                QObject::connect(publishJob, &KJob::result, feedback,
                                 [feedback, publishJob, onSuccess](KJob *) {
                    if (!feedback) return;
                    const auto env = publishJob->envelope();
                    if (publishJob->error() != 0) {
                        feedback->publishFailed(env.error.message.isEmpty() ? publishJob->errorText() : env.error.message, env);
                        return;
                    }
                    feedback->published(env);
                    onSuccess(env);
                });
                publishJob->start();
            });
        }
    }
    auto *job = new DwarfsctlJob(DwarfsctlJob::Request{title, jobArgs, timeoutMs, false}, parentWidget);
    if (parentWidget) {
        KJobWidgets::setWindow(job, parentWidget);
    }

    if (feedback) {
        QObject::connect(job, &DwarfsctlJob::buildPhase, feedback, &dwarfsplugin::BuildFeedback::phase);
        QObject::connect(feedback, &dwarfsplugin::BuildFeedback::cancelRequested,
                         job, [job] { job->kill(KJob::EmitResult); }, Qt::QueuedConnection);
    }
    if (args.value(0) == QLatin1String("extract")) {
        extraction = new ExtractionFeedback(args.value(2), false, parentWidget);
        extraction->show();
        QObject::connect(extraction, &ExtractionFeedback::cancelRequested,
                         job, [job] { job->kill(KJob::EmitResult); }, Qt::QueuedConnection);
    }
    QObject::connect(job, &KJob::result, job, [parentWidget, onSuccess, title, feedback, extraction](KJob *j) {
        auto *dj = qobject_cast<DwarfsctlJob *>(j);
        if (!dj) {
            return;
        }
        // mkdwarfs version warnings are surfaced in dialogs that have an inline banner
        // (e.g. the Build dialog). Jobs invoked from the context menu may not have a
        // suitable host UI; do not consume or display the warning here.
        const auto env = dj->envelope();

        if (dj->error() != 0) {
            if (dj->error() == KJob::KilledJobError) {
                if (feedback) feedback->cancelled();
                if (extraction) extraction->cancelled();
                return;
            }
            const QString msg = !env.error.message.isEmpty() ? env.error.message : dj->errorText();
            if (feedback) { feedback->failed(msg, env); return; }
            if (extraction) { extraction->failed(msg, env); return; }
            showErrorDialogWithDetails(parentWidget,
                                       title,
                                       msg.isEmpty() ? i18n("Operation failed") : msg,
                                       env);
            return;
        }

        const auto kind = dwarfsplugin::classifyEnvelope(env);
        if (!feedback && !extraction && kind == dwarfsplugin::NotifyKind::Warning) {
            dwarfsplugin::notifyResult(parentWidget,
                                       kind,
                                       title,
                                       i18n("Completed with warnings"));
        }

        if (feedback) feedback->complete(env);
        if (extraction) extraction->complete(env);
        onSuccess(env);
    });

    job->start();
}

static void withUiSettings(QWidget *parentWidget, const QString &title,
                            std::function<void(const dwarfsctl_kde::UiConfig &)> dispatch)
{
    auto *progress = new QProgressDialog(i18n("Loading DwarFS settings…"), i18n("Cancel"), 0, 0, parentWidget);
    progress->setWindowTitle(title);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(200);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    QObject::connect(progress, &QProgressDialog::canceled, progress, [progress] { progress->deleteLater(); });
    progress->setValue(0);
    dwarfsplugin::BackendQuery::ui(progress,
        [progress, parentWidget, title, dispatch = std::move(dispatch)](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &error) {
        if (progress->wasCanceled()) { progress->deleteLater(); return; }
        progress->hide();
        progress->deleteLater();
        if (!ok) QMessageBox::critical(parentWidget, title, error);
        else dispatch(config);
    });
}

static void runDwarfsctlJob(QWidget *parentWidget,
                           const QString &title, const QStringList &args, bool build,
                           const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    withUiSettings(parentWidget, title,
        [parentWidget, title, args, build, onSuccess](const dwarfsctl_kde::UiConfig &config) {
        runResolvedDwarfsctlJob(parentWidget, title, args,
            build ? config.build_timeout_ms : config.command_timeout_ms, onSuccess);
    });
}

static void runResolvedTrayAgentJob(QWidget *parentWidget,
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

static void runTrayAgentJob(QWidget *parentWidget,
                            const QString &title, const QString &method,
                            const QList<QVariant> &args, bool openMounted,
                            const std::function<void(const dwarfsplugin::Envelope &)> &onSuccess)
{
    withUiSettings(parentWidget, title,
        [parentWidget, title, method, args, openMounted, onSuccess](const dwarfsctl_kde::UiConfig &config) {
        runResolvedTrayAgentJob(parentWidget, title, method, args, config.command_timeout_ms,
            [onSuccess, openMounted, config](const dwarfsplugin::Envelope &env) {
            onSuccess(env);
            const auto mountpoint = envMountpoint(env);
            if (openMounted && config.open_after_mount && !mountpoint.isEmpty())
                QDesktopServices::openUrl(QUrl::fromLocalFile(mountpoint));
        });
    });
}

} // namespace

QList<QAction *> DwarfsFileItemActionPlugin::actions(const KFileItemListProperties &fileItemInfos, QWidget *parentWidget)
{
    const QString path = localPathFromSingleUrl(fileItemInfos);
    if (path.isEmpty()) {
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
                runTrayAgentJob(parentWidget,
                                i18n("Unmount %1", QFileInfo(mountedImage).fileName()),
                                QStringLiteral("UnmountByMountpoint"),
                                {mountpoint},
                                false,
                                [](const dwarfsplugin::Envelope &) {});
            });
            auto *status = new QAction(i18n("Preparing DwarFS actions…"), unmount);
            status->setEnabled(false);
            auto *retry = new QAction(i18n("Retry DwarFS Actions"), unmount);
            new MenuReadiness(unmount, {unmount}, status, retry);
            return {unmount, status, retry};
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
            if (res.preview) {
                args << QStringLiteral("--preview");
            }
            // SOURCE_DIR is a positional argument; keep it last so Clap doesn't
            // treat later options as additional positionals.
            args << path;

            runDwarfsctlJob(parentWidget,
                           res.preview ? i18n("Measure DwarFS image") : i18n("Build DwarFS image"),
                           args,
                           true,
                           [preview = res.preview](const dwarfsplugin::Envelope &env) {
                               const QString out = env.image.isEmpty() ? i18n("(unknown)") : env.image;
                               const bool created = !preview || env.details.value(QStringLiteral("published_preview")).toBool();
                               qCInfo(DWARFS_DOLPHIN_LOG) << (created ? "Build completed successfully" : "Preview measured") << "output=" << out;
                           });
        });

        rootMenu->addAction(buildAction);
        prepareMenu(rootMenu);
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
                        true,
                        [](const dwarfsplugin::Envelope &) {});
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
        runTrayAgentJob(parentWidget,
                       i18n("Mount and Open %1", imageName),
                       QStringLiteral("Mount"),
                       {imagePath},
                       true,
                       [](const dwarfsplugin::Envelope &) {});
    });

    // Unmount
    QAction *unmountAction = new QAction(QIcon::fromTheme(QStringLiteral("media-eject")), i18n("Unmount"), menu);
    unmountAction->setIconVisibleInMenu(true);
    QObject::connect(unmountAction, &QAction::triggered, parentWidget,
                     [parentWidget, imagePath, imageName, currentMountpoint]() {
        const bool haveMountpoint = !currentMountpoint.isEmpty();
        const QString method = haveMountpoint ? QStringLiteral("UnmountByMountpoint")
                                              : QStringLiteral("UnmountByImage");
        const QList<QVariant> args = haveMountpoint ? QList<QVariant>{currentMountpoint}
                                                    : QList<QVariant>{imagePath};

        runTrayAgentJob(parentWidget,
                       i18n("Unmount %1", imageName),
                       method,
                       args,
                       false,
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

    prepareMenu(menu);
    return {rootAction};
}

#include "dwarfsfileitemactionplugin.moc"
