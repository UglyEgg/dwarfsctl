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

#include <QDBusConnection>
#include <QDBusConnectionInterface>

#include <KDirWatch>
#include <KStatusNotifierItem>

#include <KIconLoader>

#include <KLocalizedString>
#include <KIconTheme>

#include "dwarfsctl_exec.h"

#include "aboutdialog.h"
#include "mount_executor.h"
#include "mount_liveness.h"
#include "placesmanager.h"
#include "settingsdialog.h"
#include "symboliciconrenderer.h"
#include "traysettings.h"

#include <unistd.h>

using dwarfsctl_kde::dwarfsctlExe;

struct MountRecord {
    QString image;
    QString mountpoint;
};

static QProcessEnvironment dwarfsctlProcEnv();

static QString dwarfsctlVersionString()
{
    QProcess p;
    p.setProcessEnvironment(dwarfsctlProcEnv());
    p.start(dwarfsctlExe(), {QStringLiteral("--version")});
    if (!p.waitForFinished(1500)) {
        p.kill();
        p.waitForFinished();
        return QStringLiteral("(unknown)");
    }

    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    if (out.isEmpty()) {
        out = QString::fromUtf8(p.readAllStandardError()).trimmed();
    }
    if (out.isEmpty()) {
        return QStringLiteral("(unknown)");
    }
    return out.section(QLatin1Char('\n'), 0, 0).trimmed();
}

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

static QStringList dwarfsctlJsonArgs(const QStringList& tail)
{
    QStringList args;
    args;
    args << tail;
    return args;
}

static QProcessEnvironment dwarfsctlProcEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Ensure dwarfsctl uses the per-session registry under the runtime directory.
    const QString reg = registryPath();
    const QString dwarfsctlDir = QFileInfo(reg).dir().absolutePath();
    const QString runtimeDir = QFileInfo(dwarfsctlDir).dir().absolutePath();
    if (!runtimeDir.isEmpty()) {
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtimeDir);
    }

    // Prevent recursion after the CLI becomes a strict D-Bus client. The tray
    // is the mount authority and may still shell out to dwarfsctl for local
    // operations.
    return env;
}

static constexpr const char* kTrayDbusService = "org.quest.entropy.dwarfsctl.Tray";
static constexpr const char* kTrayDbusPath = "/org/quest/entropy/dwarfsctl/Tray";
static constexpr const char* kTrayDbusInterface = "org.quest.entropy.dwarfsctl.Tray";

static constexpr const char* kMountsDbusPath = "/org/quest/entropy/dwarfsctl/Mounts";
static constexpr const char* kMountsDbusInterface = "org.quest.entropy.dwarfsctl.Mounts1";

class TrayDbus final : public QObject {
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
        // Return something stable and useful for debugging.
        return dwarfsctlVersionString();
    }

    void ShowAbout()
    {
        showAboutDialog(nullptr);
    }
};

class MountsDbus;

class TrayController final : public QObject, public IMountBackend {
    Q_OBJECT

    // MountsDbus exposes a small D-Bus surface that delegates to the internal
    // MountExecutor. Keep the executor accessor protected for normal code,
    // but allow the D-Bus adaptor to call into it.
    friend class MountsDbus;

public:
    explicit TrayController(QObject* parent = nullptr)
        : QObject(parent)
        , m_exec(this)
        , m_item(new KStatusNotifierItem(this))
        , m_menu(new QMenu())
        , m_watch(new KDirWatch(this))
    {
        m_item->setCategory(KStatusNotifierItem::SystemServices);
        m_item->setStatus(KStatusNotifierItem::Active);
        m_item->setContextMenu(m_menu);
        // A single tray surface avoids presenting two subtly different mount
        // managers. With one active volume, activation performs the obvious
        // action directly; otherwise it opens the same menu as a context click.
        connect(m_item, &KStatusNotifierItem::activateRequested, this, [this](bool /*active*/, const QPoint& pos) {
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
        connect(m_watch, &KDirWatch::dirty, this, &TrayController::refresh);
        connect(m_watch, &KDirWatch::created, this, &TrayController::refresh);
        connect(m_watch, &KDirWatch::deleted, this, &TrayController::refresh);

        // Initial state.
        QTimer::singleShot(0, this, &TrayController::refresh);
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

    MountExecutor* executor()
    {
        return &m_exec;
    }

signals:
    void mountsChangedJson(const QString& mountsJson);

private slots:
    void refresh()
    {
        const QList<MountRecord> before = m_mounts;
        m_mounts = fetchMounts();
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
                dwarfsctl_tray::syncPlacesEntry(true, it.value(), it.key());
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

        if (m_mounts.isEmpty()) {
            auto* none = m_menu->addAction(tr("No mounted DwarFS volumes"));
            none->setEnabled(false);
        } else {
            auto* mountedMenu = m_menu->addMenu(tr("Mounted Volumes"));
            mountedMenu->setToolTipsVisible(true);
            for (const auto& r : m_mounts) {
                const QFileInfo fi(r.image);
                QString label = fi.completeBaseName();
                if (label.isEmpty())
                    label = fi.fileName();
                auto* volume = mountedMenu->addMenu(menuLabel(label));
                volume->menuAction()->setToolTip(r.image);

                auto* open = volume->addAction(tr("Open in Dolphin"));
                connect(open, &QAction::triggered, this, [mp = r.mountpoint]() {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(mp));
                });

                auto* unmount = volume->addAction(tr("Unmount"));
                connect(unmount, &QAction::triggered, this, [this, mp = r.mountpoint]() {
                    const auto before = mountpointSet();
                    runMountAgent(m_exec.unmountByMountpoint(mp), true, before);
                });
            }

            if (m_mounts.size() > 1) {
                mountedMenu->addSeparator();
                auto* unmountAll = mountedMenu->addAction(tr("Unmount All"));
                connect(unmountAll, &QAction::triggered, this, [this]() {
                    const auto before = mountpointSet();
                    runMountAgent(m_exec.unmountAll(), true, before);
                });
            }
        }

        m_menu->addSeparator();

        auto* utility = m_menu->addAction(tr("Open DwarFS Utility…"));
        connect(utility, &QAction::triggered, this, [this]() {
            if (!QProcess::startDetached(QStringLiteral("dwarfsctl-manager"), QStringList{})) {
                m_item->showMessage(tr("Could Not Open DwarFS Utility"),
                                    tr("The application could not be started."),
                                    QStringLiteral("dialog-error"), 5000);
            }
        });

        // Recently mounted images (MRU list).
        auto* recentMenu = m_menu->addMenu(tr("Recent Images"));
        const int limit = dwarfsctl_tray::recentlyMountedLimit();
        const QStringList images = dwarfsctl_tray::recentlyMountedImages();

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
                    auto* a = recentMenu->addAction(menuLabel(dwarfsctl_tray::recentLabelForImage(img)));
                    a->setToolTip(img);
                    connect(a, &QAction::triggered, this, [this, img]() {
                        const auto before = mountpointSet();
                        runMountAgent(m_exec.mount(img), true, before);
                    });
                }
            }

            recentMenu->addSeparator();
            auto* clear = recentMenu->addAction(tr("Clear Recent Images"));
            clear->setEnabled(!images.isEmpty());
            connect(clear, &QAction::triggered, this, []() { dwarfsctl_tray::clearRecentlyMountedImages(); });
        }

        m_menu->addSeparator();

        auto* settings = m_menu->addAction(tr("Settings…"));
        connect(settings, &QAction::triggered, this, [this]() {
            SettingsDialog dialog(m_menu);
            dialog.exec();
        });

        auto* about = m_menu->addAction(tr("About DwarFSctl"));
        connect(about, &QAction::triggered, this, [this]() { showAboutDialog(m_menu); });

        auto* quit = m_menu->addAction(tr("Quit"));
        connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);
    }

private:
    QList<MountRecord> fetchMounts()
    {
        QList<MountRecord> out;

        QFile f(registryPath());
        if (!f.exists()) {
            return out;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            return out;
        }
        const QByteArray raw = f.readAll();
        f.close();

        QJsonParseError perr {};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return out;
        }

        const QJsonObject root = doc.object();
        const QJsonValue mountsV = root.value(QStringLiteral("mounts"));
        if (!mountsV.isArray()) {
            return out;
        }

        const QJsonArray mountsA = mountsV.toArray();
        for (const auto& v : mountsA) {
            if (!v.isObject()) {
                continue;
            }
            const QJsonObject o = v.toObject();
            MountRecord r;
            r.image = o.value(QStringLiteral("image")).toString();
            r.mountpoint = o.value(QStringLiteral("mountpoint")).toString();
            if (!r.image.isEmpty() && !r.mountpoint.isEmpty()) {
                // Keep only entries that are genuinely usable. Existence alone
                // cannot decide this: a dead FUSE daemon leaves the path in the
                // mount table and in the attribute cache, so the tray would show
                // a mounted icon for a mountpoint that serves nothing.
                if (dwarfsctl_tray::mountsUsable(r.mountpoint)) {
                    out.push_back(r);
                }
            }
        }

        return out;
    }

    QByteArray runDwarfsctl(const QStringList& tail, bool notify)
    {
        const QSet<QString> before = [&]() {
            QSet<QString> s;
            for (const auto& r : m_mounts) {
                s.insert(r.mountpoint);
            }
            return s;
        }();

        QProcess p;
        p.setProcessEnvironment(dwarfsctlProcEnv());
        p.start(dwarfsctlExe(), dwarfsctlJsonArgs(tail));
        p.waitForFinished(-1);

        const QByteArray rawOut = p.readAllStandardOutput();
        const QByteArray rawErr = p.readAllStandardError();
        const bool okExit = (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);

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

        if (!okExit && msg.isEmpty()) {
            msg = QString::fromUtf8(rawErr).trimmed();
            if (msg.isEmpty()) {
                msg = tr("Operation failed");
            }
        }

        if (!msg.isEmpty()) {
            if (notify) {
                m_item->showMessage(tr("DwarFSctl"), msg, QStringLiteral("dialog-error"), 5000);
            }
        }

        // Refresh state first.
        refresh();

        // If the command succeeded, remove Places entries that no longer correspond
        // to an active mount.
        if (okExit && okEnvelope) {
            QSet<QString> after;
            for (const auto& r : m_mounts) {
                after.insert(r.mountpoint);
            }
            const auto removed = before - after;
            for (const auto& mp : removed) {
                dwarfsctl_tray::removePlacesEntryForMountpoint(mp);
            }
        }

        return rawOut;
    }

    QSet<QString> mountpointSet() const
    {
        QSet<QString> s;
        for (const auto& r : m_mounts) {
            s.insert(r.mountpoint);
        }
        return s;
    }

    QByteArray runMountAgent(const QByteArray& rawOut, bool notify, const QSet<QString>& before)
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

        // If the command succeeded, remove Places entries that no longer correspond
        // to an active mount.
        if (okEnvelope) {
            QSet<QString> after;
            for (const auto& r : m_mounts) {
                after.insert(r.mountpoint);
            }
            const auto removed = before - after;
            for (const auto& mp : removed) {
                dwarfsctl_tray::removePlacesEntryForMountpoint(mp);
            }
        }
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

    MountExecutor m_exec;
    KStatusNotifierItem* m_item;
    QMenu* m_menu;
    KDirWatch* m_watch;
    QList<MountRecord> m_mounts;
};

class MountsDbus final : public QObject {
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
        if (!m_ctl) {
            return QStringLiteral("{}");
        }

        return QString::fromUtf8(m_ctl->executor()->listMounts());
    }

    QString Status(const QString& imagePath)
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->status(imagePath));
    }

    QString Mount(const QString& imagePath)
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->mount(imagePath));
    }

    QString UnmountByMountpoint(const QString& mountpoint)
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->unmountByMountpoint(mountpoint));
    }

    QString UnmountByImage(const QString& imagePath)
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->unmountByImage(imagePath));
    }

    QString UnmountAll()
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->unmountAll());
    }

    QString UnmountLast()
    {
        if (!m_ctl) {
            return QStringLiteral("{}");
        }
        return QString::fromUtf8(m_ctl->executor()->unmountLast());
    }

signals:
    void MountsChanged(const QString& mountsJson);

private:
    TrayController* m_ctl = nullptr;
};

#include "main.moc"

int main(int argc, char** argv)
{
    KIconTheme::initTheme();
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("dwarfsctl-tray"));
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
