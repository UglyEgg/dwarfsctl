// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QCursor>
#include <QLatin1Char>
#include <QStyle>
#include <QPixmap>
#include <QProcessEnvironment>
#include <QSet>
#include <QThread>
#include <QDBusContext>
#include <QDBusMessage>
#include <functional>
#include <memory>

#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include <KDirWatch>
#include <KStatusNotifierItem>

#include <KIconLoader>

#include <KLocalizedString>
#include <KIconTheme>

#include "dwarfsctl_exec.h"

#include "aboutdialog.h"
#include "backendquery.h"
#include "mount_executor.h"
#include "directmountoperation.h"
#include "imagemenuentry.h"
#include "mount_liveness.h"
#include "placesmanager.h"
#include "settingsdialog.h"
#include "symboliciconrenderer.h"
#include "traysettings.h"

#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

using dwarfsctl_kde::dwarfsctlExe;

struct MountRecord {
    QString image;
    QString mountpoint;
    QString icon;
};

static void showAboutDialog(QWidget* parent)
{
    AboutDialog dlg(parent);
    dlg.exec();
}


static QString registryPath()
{
    // Do NOT trust environment variables here: when started via D-Bus activation,
    // XDG_RUNTIME_DIR may be missing, which would cause dwarfsctl to fall back
    // to XDG_STATE_HOME and the tray would miss mounts created from Dolphin.
    //
    // Use Qt's runtime location if available; otherwise fall back to /run/user/<uid>.
    QString run = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (run.isEmpty()) {
        run = QDir(QStringLiteral("/run/user/%1")).filePath(QString::number(getuid()));
    }
    return QDir(run + "/dwarfsctl").filePath("mounts.json");
}

// The registry is a small local runtime file. Observe only its identity;
// lock-file churn in its parent directory is not a registry update.
static QByteArray registryFileIdentity(const QString &path)
{
    struct stat value {};
    if (::lstat(QFile::encodeName(path).constData(), &value) != 0) return {};
    return QByteArray::number(value.st_dev) + ':' + QByteArray::number(value.st_ino) + ':'
        + QByteArray::number(value.st_size) + ':' + QByteArray::number(value.st_mtim.tv_sec) + ':'
        + QByteArray::number(value.st_mtim.tv_nsec) + ':' + QByteArray::number(value.st_ctim.tv_sec) + ':'
        + QByteArray::number(value.st_ctim.tv_nsec);
}

static constexpr const char* kTrayDbusService = "org.quest.entropy.dwarfsctl.Tray";
static constexpr const char* kTrayDbusPath = "/org/quest/entropy/dwarfsctl/Tray";
static constexpr const char* kTrayDbusInterface = "org.quest.entropy.dwarfsctl.Tray";

static constexpr const char* kMountsDbusPath = "/org/quest/entropy/dwarfsctl/Mounts";
static constexpr const char* kMountsDbusInterface = "org.quest.entropy.dwarfsctl.Mounts1";

class TrayDbus final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.quest.entropy.dwarfsctl.Tray")

public:
    explicit TrayDbus(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

public slots:
    QString Ping()
    {
        if (!calledFromDBus()) return QStringLiteral("(unknown)");
        const auto request = message();
        const auto bus = connection();
        setDelayedReply(true);
        dwarfsplugin::BackendQuery::text(this, {QStringLiteral("--version")},
            [request, bus](const dwarfsplugin::BackendReply &reply) {
            const auto version = reply.text.trimmed().section(QLatin1Char('\n'), 0, 0).trimmed();
            bus.send(request.createReply(reply.ok && !version.isEmpty() ? version : QStringLiteral("(unknown)")));
        });
        return {};
    }

    void ShowAbout()
    {
        showAboutDialog(nullptr);
    }
};

class MountsDbus;

class TrayController final : public QObject, public IMountBackend {
    Q_OBJECT

    // D-Bus and GUI requests share the controller's single admission gate.
    friend class MountsDbus;

public:
    explicit TrayController(QObject* parent = nullptr)
        : QObject(parent)
        , m_item(new KStatusNotifierItem(this))
        , m_menu(new QMenu())
        , m_watch(new KDirWatch(this))
        , m_directOperation(new dwarfsctl_tray::DirectMountOperation(this))
    {
        m_item->setCategory(KStatusNotifierItem::SystemServices);
        m_item->setStatus(KStatusNotifierItem::Active);
        m_item->setContextMenu(m_menu);
        // A single tray surface avoids presenting two subtly different mount
        // managers. With one active volume, activation performs the obvious
        // action directly; otherwise it opens the same menu as a context click.
        connect(m_item, &KStatusNotifierItem::activateRequested, this, [this](bool /*active*/, const QPoint& pos) {
            if (m_directOperation->isActive()) { m_directOperation->showCurrent(); return; }
            refresh();
            if (m_mounts.size() == 1) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(m_mounts.constFirst().mountpoint));
                return;
            }
            rebuildMenu();
            const QPoint p = pos.isNull() ? QCursor::pos() : pos;
            m_menu->popup(p);
        });

        // Keep icon readable across theme changes.
        // QGuiApplication::paletteChanged is deprecated; track palette changes via events.
        qApp->installEventFilter(this);

        // Build menu on demand.
        connect(m_menu, &QMenu::aboutToShow, this, &TrayController::rebuildMenu);

        // Watch registry file for updates.
        const QString reg = registryPath();
        QDir().mkpath(QFileInfo(reg).dir().absolutePath());
        m_watch->addFile(reg);
        // Also watch the parent dir so creation/rotation is detected reliably.
        m_watch->addDir(QFileInfo(reg).dir().absolutePath(), KDirWatch::WatchFiles);
        m_registryIdentity = registryFileIdentity(reg);
        // Both file events and generic parent-directory events can report
        // creation/rotation. Only changed registry identity requests refresh.
        const auto registryChanged = [this, reg](const QString &) {
            const auto identity = registryFileIdentity(reg);
            if (identity == m_registryIdentity) return;
            m_registryIdentity = identity;
            refresh();
        };
        connect(m_watch, &KDirWatch::dirty, this, registryChanged);
        connect(m_watch, &KDirWatch::created, this, registryChanged);
        connect(m_watch, &KDirWatch::deleted, this, registryChanged);

        connect(m_directOperation, &dwarfsctl_tray::DirectMountOperation::finished, this,
                [this](const QByteArray &result) {
            m_authorityBusy = false;
            runMountAgent(result, false);
            rebuildMenu();
        });

        // Initial state.
        QTimer::singleShot(0, this, &TrayController::refresh);
    }

    ~TrayController() override
    {
        // Finish owned work before destroying controller state. Completion
        // callbacks are context-bound and cannot run after destruction.
        if (m_worker) { m_worker->wait(); delete m_worker; }
    }

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (obj == qApp) {
            switch (ev->type()) {
            case QEvent::ApplicationPaletteChange:
            case QEvent::ThemeChange:
                updateIcon();
                break;
            default:
                break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }

    QString mountsJson() const override
    {
        QJsonArray a;
        for (const auto& r : m_mounts) {
            QJsonObject o;
            o.insert(QStringLiteral("image"), r.image);
            o.insert(QStringLiteral("mountpoint"), r.mountpoint);
            a.push_back(o);
        }
        return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    }

signals:
    void mountsChangedJson(const QString& mountsJson);

private slots:
    void refresh()
    {
        if (m_authorityBusy) { m_refreshPending = true; return; }
        m_refreshPending = false;
        dispatch([] {
            auto envelope = QJsonDocument::fromJson(MountExecutor(nullptr).listMounts()).object();
            const auto resolve = [](const QString &path) {
                if (path.isEmpty() || path.contains(QChar::Null)) return QString{};
                const auto canonical = dwarfsctl_tray::boundedCanonicalFilePath(path);
                return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
            };
            const auto images = dwarfsctl_kde::recentImages(resolve);
            QJsonObject available;
            for (const auto &image : images) {
                struct stat source {};
                int error = 0;
                const bool exists = dwarfsctl_tray::boundedFileStat(image, &source, &error);
                // Unknown availability stays actionable; the executor reports
                // a bounded failure if mounting cannot establish the source.
                available.insert(image, exists ? QJsonValue(S_ISREG(source.st_mode))
                    : error == ENOENT || error == ENOTDIR ? QJsonValue(false) : QJsonValue(QJsonValue::Null));
            }
            QJsonObject icons;
            for (const auto &row : envelope.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounts")).toArray()) {
                const auto mount = row.toObject();
                const auto image = mount.value(QStringLiteral("image")).toString();
                icons.insert(mount.value(QStringLiteral("mountpoint")).toString(), dwarfsctl_tray::placesIconForImage(image));
            }
            envelope.insert(QStringLiteral("places_icons"), icons);
            envelope.insert(QStringLiteral("recent_images"), QJsonArray::fromStringList(images));
            envelope.insert(QStringLiteral("recent_limit"), dwarfsctl_kde::recentImageLimit(resolve));
            envelope.insert(QStringLiteral("recent_available"), available);
            return QJsonDocument(envelope).toJson();
        }, [this](const QByteArray &raw) {
            const auto envelope = QJsonDocument::fromJson(raw).object();
            m_recentImages.clear();
            for (const auto &image : envelope.value(QStringLiteral("recent_images")).toArray()) m_recentImages.append(image.toString());
            m_recentLimit = envelope.value(QStringLiteral("recent_limit")).toInt(5);
            m_recentAvailable.clear();
            const auto available = envelope.value(QStringLiteral("recent_available")).toObject();
            for (auto it = available.begin(); it != available.end(); ++it)
                if (it.value().isBool()) m_recentAvailable.insert(it.key(), it.value().toBool());
            if (!envelope.value(QStringLiteral("ok")).toBool()) return;
            QList<MountRecord> mounts;
            for (const auto &entry : envelope.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounts")).toArray()) {
                const auto row = entry.toObject();
                mounts.append({row.value(QStringLiteral("image")).toString(), row.value(QStringLiteral("mountpoint")).toString(),
                    envelope.value(QStringLiteral("places_icons")).toObject().value(row.value(QStringLiteral("mountpoint")).toString()).toString()});
            }
            applyMounts(mounts);
        });
    }

    void applyMounts(const QList<MountRecord> &mounts)
    {
        const QList<MountRecord> before = m_mounts;
        m_mounts = mounts;
        updateIcon();

        QHash<QString, QString> beforeByMountpoint;
        QHash<QString, QString> afterByMountpoint;
        for (const auto& record : before) {
            beforeByMountpoint.insert(record.mountpoint, record.image);
        }
        for (const auto& record : m_mounts) {
            afterByMountpoint.insert(record.mountpoint, record.image);
        }

        // The tray owns mount state, so it also owns the Dolphin sidebar view.
        // This covers mounts started through the CLI, MIME opener, properties
        // page, file action plugin, or the tray itself.
        for (auto it = beforeByMountpoint.cbegin(); it != beforeByMountpoint.cend(); ++it) {
            if (!afterByMountpoint.contains(it.key())) {
                dwarfsctl_tray::syncPlacesEntry(false, it.value(), it.key());
            }
        }
        for (auto it = afterByMountpoint.cbegin(); it != afterByMountpoint.cend(); ++it) {
            if (!beforeByMountpoint.contains(it.key())
                || beforeByMountpoint.value(it.key()) != it.value()) {
                QString icon;
                for (const auto &record : m_mounts) if (record.mountpoint == it.key()) { icon = record.icon; break; }
                dwarfsctl_tray::syncPlacesEntry(true, it.value(), it.key(), icon);
            }
        }

        auto keyset = [](const QList<MountRecord>& v) {
            QSet<QString> s;
            for (const auto& r : v) {
                s.insert(r.mountpoint + QLatin1Char('\n') + r.image);
            }
            return s;
        };
        if (keyset(before) != keyset(m_mounts)) {
            emit mountsChangedJson(mountsJson());
        }
    }

    void rebuildMenu()
    {
        refresh();
        m_menu->clear();
        m_menu->setToolTipsVisible(true);
        const auto menuLabel = [this](const QString &text) {
            const int width = m_menu->fontMetrics().averageCharWidth() * 48;
            return m_menu->fontMetrics().elidedText(text, Qt::ElideMiddle, width)
                .replace(QLatin1Char('&'), QStringLiteral("&&"));
        };

        const QStringList images = m_recentImages;
        QStringList peers = images;
        for (const auto &record : m_mounts)
            if (!peers.contains(record.image)) peers.append(record.image);
        const auto tooltip = [](const QString &text) {
            return QStringLiteral("<qt>%1</qt>").arg(text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>")));
        };

        if (m_mounts.isEmpty()) {
            auto* none = m_menu->addAction(tr("No mounted DwarFS volumes"));
            none->setEnabled(false);
        } else {
            auto* mountedMenu = m_menu->addMenu(tr("Mounted Volumes"));
            mountedMenu->setToolTipsVisible(true);
            for (const auto& r : m_mounts) {
                auto* volume = mountedMenu->addMenu(menuLabel(dwarfsctl_tray::imageMenuLabel(r.image, peers)));
                volume->menuAction()->setToolTip(tooltip(r.image));

                auto* open = volume->addAction(tr("Open in Dolphin"));
                connect(open, &QAction::triggered, this, [mp = r.mountpoint]() {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(mp));
                });

                auto* unmount = volume->addAction(tr("Unmount"));
                unmount->setEnabled(!m_directOperation->isActive());
                connect(unmount, &QAction::triggered, this, [this, mp = r.mountpoint]() {
                    startDirectOperation(tr("Unmount Volume"), tr("Unmounting volume…"), tr("Volume unmounted."),
                        [mp] { MountExecutor executor(nullptr); return executor.unmountByMountpoint(mp); });
                });
            }

            if (m_mounts.size() > 1) {
                mountedMenu->addSeparator();
                auto* unmountAll = mountedMenu->addAction(tr("Unmount All"));
                unmountAll->setEnabled(!m_directOperation->isActive());
                connect(unmountAll, &QAction::triggered, this, [this]() {
                    startDirectOperation(tr("Unmount All Volumes"), tr("Unmounting volumes…"), tr("Volumes unmounted."),
                        [] { MountExecutor executor(nullptr); return executor.unmountAll(); });
                });
            }
        }

        m_menu->addSeparator();

        auto* utility = m_menu->addAction(tr("Open DwarFS Utility…"));
        connect(utility, &QAction::triggered, this, [this]() { openUtility(); });

        // Shared image history is refreshed each time this menu opens.
        auto* recentMenu = m_menu->addMenu(tr("Recent Images"));
        const int limit = m_recentLimit;
        recentMenu->setEnabled(limit > 0);
        recentMenu->setToolTipsVisible(true);
        if (limit <= 0) {
            auto* disabled = recentMenu->addAction(tr("Disabled"));
            disabled->setEnabled(false);
        } else {
            if (images.isEmpty()) {
                auto* empty = recentMenu->addAction(tr("No Recent Images"));
                empty->setEnabled(false);
            } else {
                for (const auto& img : images) {
                    QString mountpoint;
                    const QString identity = QDir::cleanPath(img);
                    for (const auto &record : m_mounts) {
                        if (QDir::cleanPath(record.image) == identity) {
                            mountpoint = record.mountpoint;
                            break;
                        }
                    }
                    const auto entry = dwarfsctl_tray::recentImageMenuEntry(img, peers, mountpoint,
                        m_recentAvailable.value(img, true));
                    auto* a = recentMenu->addAction(menuLabel(entry.label));
                    a->setToolTip(tooltip(entry.tooltip));
                    a->setEnabled(!m_directOperation->isActive());
                    connect(a, &QAction::triggered, this, [this, img, entry]() {
                        if (entry.action == dwarfsctl_tray::ImageMenuAction::OpenMountedFolder) {
                            QDesktopServices::openUrl(QUrl::fromLocalFile(entry.target));
                        } else if (entry.action == dwarfsctl_tray::ImageMenuAction::LocateInUtility) {
                            openUtility(img);
                        } else {
                            startDirectOperation(tr("Mount Image"), tr("Mounting %1…").arg(QFileInfo(img).fileName()), tr("Image mounted."),
                                [img] { MountExecutor executor(nullptr); return executor.mount(img); });
                        }
                    });
                }
            }

            recentMenu->addSeparator();
            auto* clear = recentMenu->addAction(tr("Clear Recent Image History"));
            clear->setToolTip(tr("Remove saved image paths from Utility and tray history. Image files and mounted volumes are kept."));
            clear->setEnabled(!images.isEmpty());
            connect(clear, &QAction::triggered, this, [this]() {
                if (!dispatch([] {
                    return QJsonDocument(QJsonObject{{QStringLiteral("ok"), dwarfsctl_tray::clearRecentlyMountedImages()}}).toJson();
                }, [this](const QByteArray &raw) {
                    if (!QJsonDocument::fromJson(raw).object().value(QStringLiteral("ok")).toBool())
                        m_item->showMessage(tr("Could Not Clear Recent Images"),
                            tr("The image history could not be fully cleared. Check that preferences are available, then try again."),
                            QStringLiteral("dialog-error"), 5000);
                    refresh();
                })) m_item->showMessage(tr("DwarFSctl"), tr("The mount state is being checked. Try again shortly."),
                    QStringLiteral("dialog-information"), 3000);
            });
        }

        m_menu->addSeparator();

        auto* settings = m_menu->addAction(tr("Settings…"));
        connect(settings, &QAction::triggered, this, [this]() {
            SettingsDialog dialog(m_menu);
            dialog.exec();
        });

        auto* about = m_menu->addAction(tr("About DwarFSctl"));
        connect(about, &QAction::triggered, this, [this]() { showAboutDialog(m_menu); });

        if (m_directOperation->isActive()) {
            auto *current = m_menu->addAction(tr("Show Current Operation…"));
            connect(current, &QAction::triggered, m_directOperation, &dwarfsctl_tray::DirectMountOperation::showCurrent);
        }
        auto* quit = m_menu->addAction(tr("Quit"));
        quit->setEnabled(!m_directOperation->isActive());
        connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
    }

private:
    void openUtility(const QString &image = {})
    {
        const QStringList arguments = image.isEmpty() ? QStringList{} : QStringList{image};
        if (!QProcess::startDetached(QStringLiteral("dwarfsctl-manager"), arguments)) {
            m_item->showMessage(tr("Could Not Open DwarFS Utility"),
                tr("The application could not be started."), QStringLiteral("dialog-error"), 5000);
        }
    }

    void startDirectOperation(const QString &title, const QString &running, const QString &success,
                              dwarfsctl_tray::DirectMountOperation::Work work)
    {
        if (m_directOperation->isActive()) { m_directOperation->showCurrent(); return; }
        if (m_authorityBusy) {
            m_item->showMessage(tr("DwarFSctl"), tr("The mount state is being checked. Try again shortly."), QStringLiteral("dialog-information"), 3000);
            return;
        }
        m_authorityBusy = true;
        if (m_directOperation->start(title, running, success, std::move(work))) rebuildMenu();
        else m_authorityBusy = false;
    }

    QString executorBusyReply(const QString &command, const QString &image = {}) const
    {
        return QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("ok"), false}, {QStringLiteral("command"), command},
            {QStringLiteral("image"), image}, {QStringLiteral("details"), QJsonObject{}},
            {QStringLiteral("warnings"), QJsonArray{}}, {QStringLiteral("warning_objects"), QJsonArray{}},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), 1}, {QStringLiteral("kind"), QStringLiteral("Busy")},
                {QStringLiteral("message"), tr("A mount operation is already running. Wait for it to finish, then refresh the mount state.")}}}
        }).toJson(QJsonDocument::Compact));
    }

    // Admission is owned by the GUI thread. All filesystem and process work,
    // including refresh, shares this one slot; repeated refresh requests coalesce.
    bool dispatch(dwarfsctl_tray::DirectMountOperation::Work work,
                  std::function<void(const QByteArray &)> completed)
    {
        if (m_authorityBusy) return false;
        m_authorityBusy = true;
        auto result = std::make_shared<QByteArray>();
        m_worker = QThread::create([work = std::move(work), result] {
            try { *result = work(); }
            catch (...) {
                *result = QJsonDocument(QJsonObject{{QStringLiteral("ok"), false},
                    {QStringLiteral("error"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("MountState")},
                        {QStringLiteral("message"), QStringLiteral("The mount operation could not be completed.")}}}}).toJson();
            }
        });
        connect(m_worker, &QThread::finished, this, [this, result, completed = std::move(completed)] {
            m_worker->wait();
            m_worker->deleteLater();
            m_worker = nullptr;
            m_authorityBusy = false;
            completed(*result);
            if (m_refreshPending) QTimer::singleShot(0, this, &TrayController::refresh);
        });
        m_worker->start();
        return true;
    }

    QByteArray runMountAgent(const QByteArray& rawOut, bool notify)
    {
        QString msg;
        bool okEnvelope = false;
        if (!rawOut.isEmpty()) {
            QJsonParseError perr {};
            const QJsonDocument doc = QJsonDocument::fromJson(rawOut, &perr);
            if (perr.error == QJsonParseError::NoError && doc.isObject()) {
                const QJsonObject root = doc.object();
                okEnvelope = root.value("ok").toBool();
                if (!okEnvelope) {
                    const QJsonValue errV = root.value("error");
                    if (errV.isObject()) {
                        msg = errV.toObject().value("message").toString();
                    }
                    if (msg.isEmpty()) {
                        msg = tr("Operation failed");
                    }
                }
            }
        }

        if (!msg.isEmpty() && notify) {
            m_item->showMessage(tr("DwarFSctl"), msg, QStringLiteral("dialog-error"), 5000);
        }

        // Refresh state first.
        refresh();

        return rawOut;
    }

    void updateIcon()
    {
        const bool active = !m_mounts.isEmpty();
        m_item->setStatus(active ? KStatusNotifierItem::Active
                                 : KStatusNotifierItem::Passive);

        // Some StatusNotifier hosts load hicolor SVGs without applying KDE's
        // symbolic color classes. Render with the foreground in the current
        // KDE/Qt palette so the mark follows light/dark mode reliably.
        const QString path = active ? QStringLiteral(":/dwarfsctl-tray/resources/helmet-mounted.svg")
                                    : QStringLiteral(":/dwarfsctl-tray/resources/helmet-unmounted.svg");
        const int logicalSz = KIconLoader::global()->currentSize(KIconLoader::Panel);
        const qreal dpr = qApp->devicePixelRatio();
        const QSize pxSz(int(logicalSz * dpr), int(logicalSz * dpr));
        const QColor foreground = qApp->palette().color(QPalette::WindowText);
        const QImage image = dwarfsctl_tray::renderSymbolicIcon(path, foreground, pxSz);
        if (image.isNull()) {
            const QString iconName = active ? QStringLiteral("dwarfsctl-tray-mounted-symbolic")
                                            : QStringLiteral("dwarfsctl-tray-symbolic");
            m_item->setIconByName(iconName);
            m_item->setOverlayIconByName(QString());
            return;
        }

        QPixmap pm = QPixmap::fromImage(image);
        pm.setDevicePixelRatio(dpr);
        m_item->setIconByPixmap(pm);
        m_item->setOverlayIconByPixmap(QIcon());
    }

    QByteArray m_registryIdentity;
    QStringList m_recentImages;
    int m_recentLimit = 5;
    QHash<QString, bool> m_recentAvailable;
    QThread *m_worker = nullptr;
    bool m_authorityBusy = false;
    bool m_refreshPending = false;
    KStatusNotifierItem* m_item;
    QMenu* m_menu;
    KDirWatch* m_watch;
    QList<MountRecord> m_mounts;
    dwarfsctl_tray::DirectMountOperation *m_directOperation;
};

class MountsDbus final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.quest.entropy.dwarfsctl.Mounts1")

public:
    explicit MountsDbus(TrayController* ctl, QObject* parent = nullptr)
        : QObject(parent)
        , m_ctl(ctl)
    {
        if (m_ctl) {
            QObject::connect(m_ctl, &TrayController::mountsChangedJson, this, [this](const QString& j) {
                emit MountsChanged(j);
            });
        }
    }

public slots:
    int ApiMajor()
    {
        return 1;
    }

    QString ListMounts()
    {
        return request(QStringLiteral("mount_list"), QString{},
            [] { return MountExecutor(nullptr).listMounts(); });
    }

    QString Status(const QString& imagePath)
    {
        return request(QStringLiteral("mount_status"), imagePath,
            [imagePath] { return MountExecutor(nullptr).status(imagePath); });
    }

    QString Mount(const QString& imagePath)
    {
        return request(QStringLiteral("mount"), imagePath,
            [imagePath] { return MountExecutor(nullptr).mount(imagePath); });
    }

    QString UnmountByMountpoint(const QString& mountpoint)
    {
        return request(QStringLiteral("unmount"), QString{},
            [mountpoint] { return MountExecutor(nullptr).unmountByMountpoint(mountpoint); });
    }

    QString UnmountByImage(const QString& imagePath)
    {
        return request(QStringLiteral("unmount"), imagePath,
            [imagePath] { return MountExecutor(nullptr).unmountByImage(imagePath); });
    }

    QString UnmountAll()
    {
        return request(QStringLiteral("unmount_all"), QString{},
            [] { return MountExecutor(nullptr).unmountAll(); });
    }

    QString UnmountLast()
    {
        return request(QStringLiteral("unmount_last"), QString{},
            [] { return MountExecutor(nullptr).unmountLast(); });
    }


signals:
    void MountsChanged(const QString& mountsJson);

private:
    QString request(const QString &command, const QString &image,
                    dwarfsctl_tray::DirectMountOperation::Work work)
    {
        if (!m_ctl) return QStringLiteral("{}");
        if (m_ctl->m_authorityBusy) return m_ctl->executorBusyReply(command, image);
        auto reply = message().createReply();
        const auto bus = connection();
        setDelayedReply(true);
        m_ctl->dispatch(std::move(work), [this, bus, reply, command](const QByteArray &result) mutable {
            reply.setArguments({QString::fromUtf8(result)});
            bus.send(reply);
            const auto envelope = QJsonDocument::fromJson(result).object();
            const bool notDispatched = envelope.value(QStringLiteral("details")).toObject()
                .value(QStringLiteral("reason")).toString() == QLatin1String("record_not_found");
            if (command != QLatin1String("mount_list") && command != QLatin1String("mount_status") && !notDispatched)
                m_ctl->refresh();
        });
        return {};
    }
    TrayController* m_ctl = nullptr;
};

#include "main.moc"

int main(int argc, char** argv)
{
    KIconTheme::initTheme();
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("dwarfsctl-tray"));
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationDisplayName(QStringLiteral("DwarFSctl"));
    app.setOrganizationDomain(QStringLiteral("entropy.quest"));
    app.setDesktopFileName(QStringLiteral("dwarfsctl-tray"));
    app.setWindowIcon(QIcon::fromTheme(
        QStringLiteral("dwarfsctl"),
        QIcon(QStringLiteral(":/dwarfsctl-tray/resources/dwarfsctl-app.svg"))));
    KLocalizedString::setApplicationDomain("dwarfsctl");

    // D-Bus activation support (session bus). If another instance already owns the name,
    // exit quietly.
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QString::fromLatin1(kTrayDbusService))) {
        return 0;
    }

    TrayController ctl;

    TrayDbus dbus(&ctl);
    bus.registerObject(QString::fromLatin1(kTrayDbusPath), &dbus, QDBusConnection::ExportAllSlots);

    MountsDbus mounts(&ctl);
    bus.registerObject(QString::fromLatin1(kMountsDbusPath), &mounts,
                       QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals);
    return app.exec();
}
