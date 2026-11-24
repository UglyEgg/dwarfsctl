// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "managerwindow.h"
#include "imagebrowserwidget.h"
#include "imagehistory.h"
#include "imagelabel.h"
#include "mountscache.h"
#include "traydbus.h"
#include "dwarfsctljob.h"
#include <memory>
#include "visualizationwidgets.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <cstdio>
#include <QFileDialog>
#include <QLineEdit>
#include <QSettings>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeView>
#include <QFrame>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolBar>
#include <QtTest>

#include <KLocalizedString>
#include <KMessageWidget>

namespace {
// This service owns its name on the test's private bus. Cache updates use the
// same sender authentication and initial snapshot path as the application.
class MountFixture final : public QDBusVirtualObject {
    QJsonArray m_mounts;
public:
    QString introspect(const QString &) const override {
        return QStringLiteral("<interface name='org.quest.entropy.dwarfsctl.Mounts1'>"
            "<method name='ListMounts'><arg type='s' direction='out'/></method>"
            "<method name='SetMounts'><arg type='s' direction='in'/></method>"
            "<signal name='MountsChanged'><arg type='s'/></signal></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
        if (message.member() == QLatin1String("ListMounts")) {
            const QJsonObject envelope{{QStringLiteral("ok"), true},
                {QStringLiteral("details"), QJsonObject{{QStringLiteral("mounts"), m_mounts}}},
                {QStringLiteral("error"), QJsonValue(QJsonValue::Null)}};
            return bus.send(message.createReply(QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact))));
        }
        if (message.member() != QLatin1String("SetMounts")) return false;
        const auto document = QJsonDocument::fromJson(message.arguments().value(0).toString().toUtf8());
        if (!document.isArray()) return bus.send(message.createErrorReply(QStringLiteral("org.test.Invalid"), QStringLiteral("Expected mounts array")));
        m_mounts = document.array();
        auto signal = QDBusMessage::createSignal(QString::fromLatin1(dwarfsplugin::kMountsDbusPath),
            QString::fromLatin1(dwarfsplugin::kMountsDbusInterface), QStringLiteral("MountsChanged"));
        signal << QString::fromUtf8(QJsonDocument(m_mounts).toJson(QJsonDocument::Compact));
        bus.send(signal);
        return bus.send(message.createReply());
    }
};

// Each integration case gets an isolated recent list, preferences and backend.
class BrowserBackend final {
public:
    BrowserBackend()
        : oldConfig(qgetenv("XDG_CONFIG_HOME")), oldBackend(qgetenv("DWARFSCTL_BIN"))
    {
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("DWARFSCTL_BIN", directory.filePath(QStringLiteral("backend")).toUtf8());
    }
    ~BrowserBackend()
    {
        if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", oldConfig);
        if (oldBackend.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", oldBackend);
    }
    bool prepare()
    {
        QFile backend(directory.filePath(QStringLiteral("backend")));
        if (!directory.isValid() || !backend.open(QIODevice::WriteOnly)) return false;
        backend.write(R"PY(#!/usr/bin/python3
import json,sys,time
from pathlib import Path
with open(sys.argv[0]+'.log','a') as log: log.write(json.dumps(sys.argv[1:])+'\n')
command=sys.argv[1]
if command=='status' and sys.argv[2].endswith('/racing.dwarfs'):
    counter=Path(sys.argv[0]+'.race-count')
    count=int(counter.read_text())+1 if counter.exists() else 1
    counter.write_text(str(count))
    if count==1:
        Path(sys.argv[0]+'.race-started').write_text('first status started')
        for attempt in range(300):
            if Path(sys.argv[0]+'.race-release').exists(): break
            time.sleep(0.02)
        Path(sys.argv[0]+'.race-finished').write_text('first result released')
        print(json.dumps(dict(ok=False,command='status',details={},error=dict(code=9,kind='Reader',message='stale first status failure'))))
        sys.exit(9)
if command=='status' and Path(sys.argv[0]+'.usage-status-fail').exists():
    Path(sys.argv[0]+'.usage-status-fail').unlink()
    print(json.dumps(dict(ok=False,command='status',details={},error=dict(code=9,kind='Reader',message='temporary status failure'))))
    sys.exit(9)
if command=='usage' and sys.argv[2].endswith('/usage-delayed.dwarfs'):
    Path(sys.argv[0]+'.usage-started').write_text('usage started')
    for attempt in range(300):
        if Path(sys.argv[0]+'.usage-release').exists(): break
        time.sleep(0.01)
if command=='config':
    details=dict(command_timeout_ms=300000,build_timeout_ms=0,open_after_mount=True)
elif command=='ls':
    time.sleep(0.15)
    details=dict(entries=[dict(name='<literal>[*] Ω.dwarfs',path='/<literal>[*] Ω.dwarfs',mode=33188,is_dir=False,size=42)])
elif command=='usage':
    details=dict(schema='dwarfsctl.usage.v1',root=dict(name='',path='/',type='directory',size=84 if sys.argv[2].endswith('/usage-delayed.dwarfs') else 42,files=1,directories=0,children=[]))
else:
    if command=='status' and sys.argv[2].endswith('/delayed.dwarfs'): time.sleep(0.35)
    details=dict(mounted=False)
print(json.dumps(dict(ok=True,command=command,details=details)))
)PY");
        backend.close();
        return backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    QString image(const QString &name)
    {
        const auto path = directory.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile image(path);
        if (!image.open(QIODevice::WriteOnly)) return {};
        image.write("test image");
        return path;
    }
    QSettings settings() const
    {
        const auto folder = directory.filePath(QStringLiteral("dwarfsctl"));
        QDir().mkpath(folder);
        return QSettings(QDir(folder).filePath(QStringLiteral("manager.ini")), QSettings::IniFormat);
    }
    QList<QJsonArray> invocations() const
    {
        QFile log(directory.filePath(QStringLiteral("backend.log")));
        QList<QJsonArray> result;
        if (log.open(QIODevice::ReadOnly)) {
            for (const auto &line : log.readAll().split('\n'))
                if (!line.isEmpty()) result.append(QJsonDocument::fromJson(line).array());
        }
        return result;
    }
    QTemporaryDir directory;
    QByteArray oldConfig;
    QByteArray oldBackend;
};
}

class ManagerWindowTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_testConfig;
    QByteArray m_originalConfig;
    QProcess m_mountAgent;
    QListWidget *imageList(DwarfsManagerWindow &window) const
    {
        return window.findChild<QListWidget *>(QStringLiteral("imageList"));
    }
    QListWidgetItem *imageItem(DwarfsManagerWindow &window, const QString &path) const
    {
        auto *list = imageList(window);
        for (int row = 0; row < list->count(); ++row)
            if (list->item(row)->data(Qt::UserRole + 1).toString() == path) return list->item(row);
        return nullptr;
    }
    QAction *action(DwarfsManagerWindow &window, const char *name) const
    {
        return window.findChild<QAction *>(QString::fromLatin1(name));
    }
    void setCachedMounts(const QString &image = {}, const QString &mountpoint = {})
    {
        QJsonArray entries;
        if (!image.isEmpty()) entries.append(QJsonObject{{QStringLiteral("image"), image}, {QStringLiteral("mountpoint"), mountpoint}});
        auto *cache = dwarfsplugin::MountsCache::instance();
        cache->ensureStarted();
        auto request = QDBusMessage::createMethodCall(QString::fromLatin1(dwarfsplugin::kTrayDbusService),
            QString::fromLatin1(dwarfsplugin::kMountsDbusPath), QString::fromLatin1(dwarfsplugin::kMountsDbusInterface),
            QStringLiteral("SetMounts"));
        request << QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact));
        const auto reply = QDBusConnection::sessionBus().call(request, QDBus::Block, 1000);
        QVERIFY2(reply.type() != QDBusMessage::ErrorMessage, qPrintable(reply.errorMessage()));
        QHash<QString, QString> expected;
        if (!image.isEmpty()) expected.insert(image, mountpoint);
        QTRY_VERIFY_WITH_TIMEOUT(cache->isKnown(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(cache->mounts(), expected, 3000);
    }
    void recoveryPageIsFocused(DwarfsManagerWindow &window, bool mounted)
    {
        auto *page = window.findChild<QWidget *>(QStringLiteral("unavailableImagePage"));
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(page && views);
        QVERIFY(!page->isHidden());
        QVERIFY(page->isVisibleTo(&window));
        QVERIFY(views->isHidden());
        QVERIFY(!window.findChild<QWidget *>(QStringLiteral("compressionOverview"))->isVisibleTo(&window));
        QString guidance;
        for (auto *label : page->findChildren<QLabel *>()) {
            QCOMPARE(label->textFormat(), Qt::PlainText);
            guidance += label->text() + QLatin1Char(' ');
        }
        QVERIFY(guidance.contains(mounted ? QStringLiteral("Open Mounted Folder") : QStringLiteral("Locate")));
        if (mounted) QVERIFY(guidance.contains(QStringLiteral("Unmount")));
        else QVERIFY(guidance.contains(QStringLiteral("image file unchanged")));
    }
    void sourceActionsDisabled(DwarfsManagerWindow &window)
    {
        for (const auto *name : {"extractAction", "extractSelectedAction", "verifyAction", "checksumAction"}) {
            QVERIFY(action(window, name));
            QVERIFY(!action(window, name)->isEnabled());
        }
    }

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("dwarfsctl");
        QVERIFY(qEnvironmentVariable("DWARFSCTL_TEST_PRIVATE_BUS") == QLatin1String("1"));
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(!bus.interface()->isServiceRegistered(QString::fromLatin1(dwarfsplugin::kTrayDbusService)));
        m_mountAgent.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--mount-fixture")});
        QVERIFY(m_mountAgent.waitForStarted(3000));
        QVERIFY(m_mountAgent.waitForReadyRead(3000));
        QCOMPARE(m_mountAgent.readLine().trimmed(), QByteArray("ready"));
    }
    void cleanupTestCase()
    {
        m_mountAgent.terminate();
        if (!m_mountAgent.waitForFinished(3000)) { m_mountAgent.kill(); QVERIFY(m_mountAgent.waitForFinished(1000)); }
        QTRY_VERIFY_WITH_TIMEOUT(!dwarfsplugin::MountsCache::instance()->isKnown(), 3000);
    }

    void init()
    {
        m_originalConfig = qgetenv("XDG_CONFIG_HOME");
        m_testConfig = std::make_unique<QTemporaryDir>();
        QVERIFY(m_testConfig->isValid());
        qputenv("XDG_CONFIG_HOME", m_testConfig->path().toUtf8());
        setCachedMounts();
    }
    void cleanup()
    {
        setCachedMounts();
        if (m_originalConfig.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", m_originalConfig);
        m_testConfig.reset();
    }

    void exposesPrimarySurfaces()
    {
        DwarfsManagerWindow window;
        QVERIFY(window.findChild<QListWidget *>(QStringLiteral("imageList")));
        QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("detailPages")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("openImageAction")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("createImageAction")));
        auto *about = window.findChild<QAction *>(QStringLiteral("aboutAction"));
        auto *aboutButton = window.findChild<QToolButton *>(QStringLiteral("aboutButton"));
        QVERIFY(about && aboutButton);
        QCOMPARE(aboutButton->defaultAction(), about);
        QVERIFY(window.findChild<QToolBar *>(QStringLiteral("mainToolBar"))->isAncestorOf(aboutButton));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("forgetImageAction")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("clearRecentImagesAction")));
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("forgetImageButton")));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("sidebarSummary")));
        QVERIFY(window.findChild<QFrame *>(QStringLiteral("mountStateBox")));
        QVERIFY(window.findChild<QFrame *>(QStringLiteral("mountLocationStrip")));
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(views);
        QCOMPARE(views->count(), 3);
        QCOMPARE(views->tabText(1), QStringLiteral("Files"));
        QCOMPARE(views->tabText(2), QStringLiteral("Size Map"));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("extractSelectedAction")));
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("compressionOverview")));
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        auto *mount = window.findChild<QAction *>(QStringLiteral("mountAction"));
        auto *browse = window.findChild<QAction *>(QStringLiteral("browseAction"));
        auto *extract = window.findChild<QAction *>(QStringLiteral("extractAction"));
        QVERIFY(mount && browse && extract);
        QVERIFY(!mount->isEnabled());
        QVERIFY(!browse->isEnabled());
        QVERIFY(!extract->isEnabled());
    }

    void longImageNamesRemainReadableAtMinimumWidth()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString name = QStringLiteral("Project archive with supporting documents ").repeated(5).trimmed();
        const QString path = directory.filePath(name + QStringLiteral(".dwarfs"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("invalid image");
        file.close();
        DwarfsManagerWindow window;
        window.resize(880, 600);
        window.openImages({path});
        window.show();
        QTest::qWait(100);
        auto *title = window.findChild<QLabel *>(QStringLiteral("imageName"));
        auto *state = window.findChild<QFrame *>(QStringLiteral("mountStateBox"));
        QVERIFY(title && state);
        QCOMPARE(title->toolTip(), name);
        QCOMPARE(title->accessibleName(), name);
        QVERIFY(title->text() != name);
        QVERIFY(title->geometry().right() < state->geometry().left());
        QCOMPARE(window.width(), 880);
    }

    void recentImageCanBeRemovedWithoutTouchingTheFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sample.dwarfs"));
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write("not-a-real-image"), 16);
        image.close();

        DwarfsManagerWindow window;
        window.openImages({path});
        auto *images = window.findChild<QListWidget *>(QStringLiteral("imageList"));
        auto *forget = window.findChild<QAction *>(QStringLiteral("forgetImageAction"));
        auto *summary = window.findChild<QLabel *>(QStringLiteral("sidebarSummary"));
        QVERIFY(images && forget && summary);
        QCOMPARE(images->count(), 2); // Section heading plus the image.
        QVERIFY(forget->isEnabled());
        QCOMPARE(summary->text(), i18n("1 image"));

        forget->trigger();
        QCOMPARE(images->count(), 0);
        QCOMPARE(summary->text(), i18n("No images added"));
        QVERIFY(QFileInfo::exists(path));
    }

    void startupSelectsAnImageInsteadOfItsSectionHeader()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto path = backend.image(QStringLiteral("startup.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(path));
        DwarfsManagerWindow window;
        QVERIFY(imageList(window)->currentItem());
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), path);
        QVERIFY(imageList(window)->currentItem()->flags().testFlag(Qt::ItemIsSelectable));
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "extractAction")->isEnabled(), 5000);
    }
    void unavailableHistoryRemainsVisibleAndCanBeLocated()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto missing = backend.directory.filePath(QStringLiteral("missing.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(missing));
        { auto settings = backend.settings(); settings.setValue(QStringLiteral("view/selected"), QStringLiteral("files")); settings.sync(); }
        DwarfsManagerWindow window;
        QVERIFY(imageItem(window, missing));
        QVERIFY(imageItem(window, missing)->text().contains(QStringLiteral("Unavailable")));
        recoveryPageIsFocused(window, false);
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QCOMPARE(views->currentIndex(), 1);
        QCOMPARE(backend.settings().value(QStringLiteral("view/selected")).toString(), QStringLiteral("files"));
        QVERIFY(action(window, "locateImageAction")->isEnabled());
        QVERIFY(!action(window, "mountAction")->isEnabled());
        QVERIFY(!action(window, "browseAction")->isEnabled());
        QVERIFY(!action(window, "showImageInFolderAction")->isEnabled());
        sourceActionsDisabled(window);
        QVERIFY(window.findChild<ImageBrowserWidget *>()->imagePath().isEmpty());
        QVERIFY(backend.invocations().isEmpty()); // No backend work for unavailable sources.
        QFile returnedSource(missing);
        QVERIFY(returnedSource.open(QIODevice::WriteOnly));
        returnedSource.write("source returned");
        returnedSource.close();
        QVERIFY(QMetaObject::invokeMethod(&window, "refreshImages"));
        auto *browser = window.findChild<ImageBrowserWidget *>();
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady(), 5000);
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("unavailableImagePage"))->isHidden());
        QVERIFY(!views->isHidden());
        QVERIFY(views->isEnabled());
        QCOMPARE(views->currentIndex(), 1);
        QCOMPARE(backend.settings().value(QStringLiteral("view/selected")).toString(), QStringLiteral("files"));
    }
    void cachedMountSurvivesMissingSourceAndHistoryRemoval()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto missing = backend.directory.filePath(QStringLiteral("mounted-source.dwarfs"));
        const auto preserved = backend.image(QStringLiteral("preserved.dwarfs"));
        const QString mountpoint = QStringLiteral("/synthetic/mounted-volume");
        QVERIFY(dwarfsctl_kde::rememberImage(preserved));
        QVERIFY(dwarfsctl_kde::rememberImage(missing));
        setCachedMounts(missing, mountpoint);
        DwarfsManagerWindow window;
        QVERIFY(imageItem(window, missing));
        QVERIFY(imageItem(window, missing)->text().contains(QStringLiteral("Source unavailable")));
        recoveryPageIsFocused(window, true);
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), missing);
        QVERIFY(action(window, "mountAction")->isEnabled());
        QCOMPARE(action(window, "mountAction")->text(), QStringLiteral("Unmount"));
        QVERIFY(action(window, "browseAction")->isEnabled());
        QCOMPARE(action(window, "browseAction")->text(), QStringLiteral("Open Mounted Folder"));
        QVERIFY(!action(window, "locateImageAction")->isEnabled());
        sourceActionsDisabled(window);
        action(window, "forgetImageAction")->trigger();
        QVERIFY(!dwarfsctl_kde::recentImages().contains(missing));
        QVERIFY(imageItem(window, missing));
        QVERIFY(action(window, "mountAction")->isEnabled());
        action(window, "clearRecentImagesAction")->trigger();
        QVERIFY(dwarfsctl_kde::recentImages().isEmpty());
        QVERIFY(imageItem(window, missing));
        QCOMPARE(imageList(window)->count(), 2); // Mounted heading plus its retained image.
        QFile file(preserved);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("test image"));
        QVERIFY(!QFileInfo::exists(missing));
        QVERIFY(backend.invocations().isEmpty());
    }
    void historyOffStillAllowsSessionImages()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        QVERIFY(dwarfsctl_kde::setRecentImageLimit(0));
        const auto path = backend.image(QStringLiteral("session.dwarfs"));
        DwarfsManagerWindow window;
        window.openImages({path});
        QVERIFY(imageItem(window, path));
        QCOMPARE(imageList(window)->item(0)->text(), QStringLiteral("Open Images"));
        QVERIFY(dwarfsctl_kde::recentImages().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(action(window, "extractAction")->isEnabled(), 5000);
        QVERIFY(!action(window, "clearRecentImagesAction")->isEnabled());
        action(window, "forgetImageAction")->trigger();
        QCOMPARE(imageList(window)->count(), 0);
        QVERIFY(QFileInfo::exists(path));
    }
    void equalBasenamesShowTheirDifferentParentFolders()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.image(QStringLiteral("one/archive.dwarfs"));
        const auto second = backend.image(QStringLiteral("two/archive.dwarfs"));
        DwarfsManagerWindow window;
        window.openImages({first, second});
        QVERIFY(imageItem(window, first));
        QVERIFY(imageItem(window, second));
        QVERIFY(imageItem(window, first)->text().contains(QStringLiteral("\none")));
        QVERIFY(imageItem(window, second)->text().contains(QStringLiteral("\ntwo")));
        QVERIFY(imageItem(window, first)->text() != imageItem(window, second)->text());
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), second);
    }
    void locateRepairsTheRememberedPath_data()
    {
        QTest::addColumn<bool>("originalReturns");
        QTest::newRow("repair missing path") << false;
        QTest::newRow("keep original that returns during chooser") << true;
    }
    void locateRepairsTheRememberedPath()
    {
        QFETCH(bool, originalReturns);
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto missing = backend.directory.filePath(QStringLiteral("old-location.dwarfs"));
        const auto replacement = backend.image(QStringLiteral("new-location.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(missing));
        { auto settings = backend.settings(); settings.setValue(QStringLiteral("view/selected"), QStringLiteral("files")); settings.sync(); }
        DwarfsManagerWindow window;
        window.show();
        recoveryPageIsFocused(window, false);
        QVERIFY(action(window, "locateImageAction")->isEnabled());
        const bool previous = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restore = qScopeGuard([previous] { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, previous); });
        bool chooserOpened = false;
        bool timedOut = false;
        bool originalRestored = false;
        QTimer picker;
        connect(&picker, &QTimer::timeout, &picker, [&] {
            for (auto *dialog : window.findChildren<QFileDialog *>()) {
                if (!dialog->isVisible()) continue;
                chooserOpened = true;
                if (originalReturns && !originalRestored) {
                    QFile original(missing);
                    QVERIFY(original.open(QIODevice::WriteOnly));
                    original.write("original returned");
                    original.close();
                    originalRestored = true;
                    continue; // Let the chooser observe this one filesystem change.
                }
                dialog->selectFile(replacement);
                if (auto *filename = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) filename->setText(replacement);
                if (dialog->selectedFiles() == QStringList{replacement})
                    static_cast<QDialog *>(dialog)->done(QDialog::Accepted);
            }
        });
        QTimer::singleShot(1500, &window, [&] {
            for (auto *dialog : window.findChildren<QFileDialog *>()) {
                if (!dialog->isVisible()) continue;
                timedOut = true;
                dialog->reject();
            }
        });
        picker.start(10);
        action(window, "locateImageAction")->trigger();
        picker.stop();
        QVERIFY(chooserOpened);
        QVERIFY(!timedOut);
        if (originalReturns) {
            QVERIFY(dwarfsctl_kde::recentImages().contains(missing));
            QVERIFY(!dwarfsctl_kde::recentImages().contains(replacement));
            QVERIFY(imageItem(window, missing));
            QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), missing);
            QFile original(missing);
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(), QByteArray("original returned"));
        } else {
            QVERIFY(!dwarfsctl_kde::recentImages().contains(missing));
            QVERIFY(dwarfsctl_kde::recentImages().contains(replacement));
            QVERIFY(!imageItem(window, missing));
            QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), replacement);
            QVERIFY(!QFileInfo::exists(missing));
        }
        QVERIFY(QFileInfo::exists(replacement));
        QTRY_VERIFY_WITH_TIMEOUT(window.findChild<ImageBrowserWidget *>()->isReady(), 5000);
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("unavailableImagePage"))->isHidden());
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(!views->isHidden());
        QCOMPARE(views->currentIndex(), 1);
        QCOMPARE(backend.settings().value(QStringLiteral("view/selected")).toString(), QStringLiteral("files"));
    }
    void sharedHistoryChangesRefreshTheOpenWindow()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.directory.filePath(QStringLiteral("first-unavailable.dwarfs"));
        const auto second = backend.directory.filePath(QStringLiteral("second-unavailable.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(first));
        DwarfsManagerWindow window;
        QVERIFY(imageItem(window, first));
        QVERIFY(dwarfsctl_kde::rememberImage(second));
        QTRY_VERIFY_WITH_TIMEOUT(imageItem(window, second), 5000);
        QVERIFY(dwarfsctl_kde::forgetImage(first));
        QTRY_VERIFY_WITH_TIMEOUT(!imageItem(window, first), 5000);
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), second);
        QVERIFY(action(window, "locateImageAction")->isEnabled());
        QVERIFY(dwarfsctl_kde::clearRecentImages());
        QTRY_COMPARE_WITH_TIMEOUT(imageList(window)->count(), 0, 5000);
    }
    void staleStatusCompletionCannotLeaveUnavailableSelectionDisabled()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto available = backend.image(QStringLiteral("delayed.dwarfs"));
        const auto missing = backend.directory.filePath(QStringLiteral("missing-after-delay.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(missing));
        QVERIFY(dwarfsctl_kde::rememberImage(available));
        DwarfsManagerWindow window;
        QVERIFY(imageItem(window, available));
        QVERIFY(imageItem(window, missing));
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), available);
        imageList(window)->setCurrentItem(imageItem(window, missing));
        QVERIFY(action(window, "locateImageAction")->isEnabled()); // Recovery never waits for the other image's read.
        sourceActionsDisabled(window);
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), missing);
        QTest::qWait(450);
        QVERIFY(action(window, "locateImageAction")->isEnabled());
        QVERIFY(window.findChild<ImageBrowserWidget *>()->imagePath().isEmpty());
    }

    void returningToAnImageRejectsItsEarlierStatusResult()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.image(QStringLiteral("racing.dwarfs"));
        const auto other = backend.image(QStringLiteral("other.dwarfs"));
        QVERIFY(dwarfsctl_kde::rememberImage(other));
        QVERIFY(dwarfsctl_kde::rememberImage(first));
        { auto settings = backend.settings(); settings.setValue(QStringLiteral("view/selected"), QStringLiteral("files")); settings.sync(); }
        DwarfsManagerWindow window;
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(backend.directory.filePath(QStringLiteral("backend.race-started"))), 5000);
        const auto originalJobs = window.findChildren<dwarfsplugin::DwarfsctlJob *>();
        QCOMPARE(originalJobs.size(), 1);
        QPointer<dwarfsplugin::DwarfsctlJob> originalJob = originalJobs.constFirst();
        QSignalSpy originalFinished(originalJob, &KJob::result);
        imageList(window)->setCurrentItem(imageItem(window, other));
        imageList(window)->setCurrentItem(imageItem(window, first));
        auto *browser = window.findChild<ImageBrowserWidget *>();
        QVERIFY(browser);
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady() && browser->imagePath() == first, 5000);
        QVERIFY(action(window, "extractAction")->isEnabled());
        QFile release(backend.directory.filePath(QStringLiteral("backend.race-release")));
        QVERIFY(release.open(QIODevice::WriteOnly));
        release.close();
        QTRY_VERIFY_WITH_TIMEOUT(originalJob.isNull(), 5000); // Obsolete owned read is stopped and released quietly.
        QVERIFY(originalFinished.count() <= 1);
        QVERIFY(window.findChild<KMessageWidget *>(QStringLiteral("inlineError"))->isHidden());
        QVERIFY(browser->isReady());
        QCOMPARE(browser->imagePath(), first);
        QVERIFY(action(window, "extractAction")->isEnabled());
        QCOMPARE(imageList(window)->currentItem()->data(Qt::UserRole + 1).toString(), first);
    }

    void returningToAnImageReloadsAnInvalidatedUsageMap()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.image(QStringLiteral("usage-first.dwarfs"));
        const auto delayed = backend.image(QStringLiteral("usage-delayed.dwarfs"));
        const auto identity = dwarfsctl_kde::fileIdentity(first);
        QVERIFY(identity);
        DwarfsManagerWindow window;
        window.findChild<QTabWidget *>(QStringLiteral("imageViews"))->setCurrentIndex(2);
        auto *rings = static_cast<ContentRingsWidget *>(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        window.openImages({first});
        QTRY_VERIFY_WITH_TIMEOUT(rings->hasUsage(), 5000);
        QCOMPARE(rings->totalBytes(), quint64(42));
        window.openImages({delayed});
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(backend.directory.filePath(QStringLiteral("backend.usage-started"))), 5000);
        QVERIFY(!rings->hasUsage());
        imageList(window)->setCurrentItem(imageItem(window, first));
        QTRY_VERIFY_WITH_TIMEOUT(rings->hasUsage(), 5000);
        QCOMPARE(rings->totalBytes(), quint64(42));
        int firstUsageCalls = 0;
        for (const auto &call : backend.invocations())
            if (call.at(0).toString() == QLatin1String("usage") && call.at(1).toString() == first) ++firstUsageCalls;
        QCOMPARE(firstUsageCalls, 2);
        QFile release(backend.directory.filePath(QStringLiteral("backend.usage-release")));
        QVERIFY(release.open(QIODevice::WriteOnly)); release.close();
        QTest::qWait(150);
        QVERIFY(rings->hasUsage());
        QCOMPARE(rings->totalBytes(), quint64(42));
        QVERIFY(dwarfsctl_kde::fileIdentity(first) == identity);
    }
    void usageRecoversAfterStatusErrorWithoutChangingTheImage()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.image(QStringLiteral("usage-recovery.dwarfs"));
        const auto identity = dwarfsctl_kde::fileIdentity(first);
        DwarfsManagerWindow window;
        window.findChild<QTabWidget *>(QStringLiteral("imageViews"))->setCurrentIndex(2);
        auto *rings = static_cast<ContentRingsWidget *>(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        window.openImages({first});
        QTRY_VERIFY_WITH_TIMEOUT(rings->hasUsage(), 5000);
        QFile fail(backend.directory.filePath(QStringLiteral("backend.usage-status-fail")));
        QVERIFY(fail.open(QIODevice::WriteOnly)); fail.close();
        QVERIFY(QMetaObject::invokeMethod(&window, "refreshImages"));
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<KMessageWidget *>(QStringLiteral("inlineError"))->isHidden(), 5000);
        QVERIFY(!rings->hasUsage());
        QVERIFY(QMetaObject::invokeMethod(&window, "refreshImages"));
        QTRY_VERIFY_WITH_TIMEOUT(rings->hasUsage(), 5000);
        QCOMPARE(rings->totalBytes(), quint64(42));
        QVERIFY(dwarfsctl_kde::fileIdentity(first) == identity);
    }

    void visualizationsUseExactAggregateData()
    {
        CompressionOverviewWidget compression;
        compression.setValues(105640000, 100590000);
        QCOMPARE(compression.originalBytes(), 105640000);
        QCOMPARE(compression.imageBytes(), 100590000);

        ContentRingsWidget rings;
        const QJsonObject usage{
            {QStringLiteral("schema"), QStringLiteral("dwarfsctl.usage.v1")},
            {QStringLiteral("root"), QJsonObject{
                {QStringLiteral("name"), QString()},
                {QStringLiteral("path"), QStringLiteral("/")},
                {QStringLiteral("type"), QStringLiteral("directory")},
                {QStringLiteral("size"), 42},
                {QStringLiteral("files"), 2},
                {QStringLiteral("directories"), 1},
                {QStringLiteral("children"), QJsonArray{
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("Artwork")},
                                {QStringLiteral("path"), QStringLiteral("/Artwork")},
                                {QStringLiteral("type"), QStringLiteral("directory")},
                                {QStringLiteral("size"), 42},
                                {QStringLiteral("files"), 2},
                                {QStringLiteral("directories"), 1}},
                }},
            }},
        };
        QVERIFY(rings.setUsage(usage));
        QCOMPARE(rings.totalBytes(), 42);
        QCOMPARE(rings.topLevelCount(), 1);
        QVERIFY(rings.hasUsage());
        rings.setLoading();
        QVERIFY(!rings.hasUsage());
        QCOMPARE(rings.totalBytes(), quint64(0));
        QVERIFY(rings.setUsage(usage));
        rings.setError(QStringLiteral("Temporary failure"));
        QVERIFY(!rings.hasUsage());
        QCOMPARE(rings.totalBytes(), quint64(0));
    }

    void previousContentsPreferenceMigratesToSizeMap()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        {
            auto settings = backend.settings();
            settings.setValue(QStringLiteral("view/current"), 1);
            settings.sync();
        }
        {
            DwarfsManagerWindow window;
            auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
            QCOMPARE(views->currentIndex(), 2);
            QCOMPARE(views->tabText(views->currentIndex()), QStringLiteral("Size Map"));
            views->setCurrentIndex(1);
        }
        {
            auto settings = backend.settings();
            QCOMPARE(settings.value(QStringLiteral("view/selected")).toString(), QStringLiteral("files"));
        }
        DwarfsManagerWindow restored;
        QCOMPARE(restored.findChild<QTabWidget *>(QStringLiteral("imageViews"))->currentIndex(), 1);
    }

    void filesExtractionRequiresReadySelectionAndRejectsStaleOrEmptyRequests()
    {
        BrowserBackend backend;
        QVERIFY(backend.prepare());
        const auto first = backend.image(QStringLiteral("first.dwarfs"));
        const auto second = backend.image(QStringLiteral("second.dwarfs"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        DwarfsManagerWindow window;
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        auto *browser = window.findChild<ImageBrowserWidget *>();
        auto *tree = window.findChild<QTreeView *>(QStringLiteral("imageBrowserView"));
        auto *selected = window.findChild<QAction *>(QStringLiteral("extractSelectedAction"));
        QVERIFY(views && browser && tree && selected);
        views->setCurrentIndex(1);
        window.openImages({first});
        QVERIFY(!selected->isEnabled());
        QVERIFY(!browser->isReady());
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady(), 5000);
        QVERIFY(!selected->isEnabled());
        const auto choose = [&] {
            tree->selectionModel()->select(tree->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        };
        choose();
        QCOMPARE(browser->selectedPaths(), QStringList{QStringLiteral("/<literal>[*] Ω.dwarfs")});
        QVERIFY(selected->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&window, "refreshImages"));
        QTRY_VERIFY_WITH_TIMEOUT(selected->isEnabled(), 5000);
        QCOMPARE(browser->selectedPaths(), QStringList{QStringLiteral("/<literal>[*] Ω.dwarfs")});
        views->setCurrentIndex(0);
        QVERIFY(!selected->isEnabled());
        views->setCurrentIndex(1);
        QVERIFY(selected->isEnabled());

        // A defensive call must return before opening a destination chooser.
        // Reject any unexpected chooser promptly so a regression cannot hang CI.
        const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        const auto restoreDialogs = qScopeGuard([nativeDialogsDisabled] {
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
        });
        bool destinationRequested = false;
        QTimer dialogGuard;
        connect(&dialogGuard, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
                    destinationRequested = true;
                    dialog->reject();
                }
            }
        });
        dialogGuard.start(10);
        tree->selectionModel()->clearSelection();
        QVERIFY(!selected->isEnabled());
        selected->setEnabled(true); // Exercise the slot's empty-selection guard too.
        QVERIFY(QMetaObject::invokeMethod(&window, "extractSelected"));
        QVERIFY(!destinationRequested);
        choose();
        QVERIFY(selected->isEnabled());
        QFile modified(first);
        QVERIFY(modified.open(QIODevice::Append));
        modified.write("new image revision");
        modified.close();
        QVERIFY(!browser->isReady());
        QVERIFY(browser->selectedPaths().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(&window, "extractSelected"));
        QVERIFY(!destinationRequested);
        QVERIFY(!selected->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady(), 5000);
        QVERIFY(browser->selectedPaths().isEmpty());
        choose();
        QVERIFY(selected->isEnabled());
        window.openImages({second});
        QVERIFY(browser->selectedPaths().isEmpty());
        QVERIFY(!selected->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady() && browser->imagePath() == second, 5000);
        QVERIFY(browser->selectedPaths().isEmpty());
        QVERIFY(!selected->isEnabled());
        dialogGuard.stop();
        const auto calls = backend.invocations();
        QVERIFY(!calls.isEmpty());
        bool listed = false;
        for (const auto &call : calls) {
            QVERIFY(call.first().toString() != QStringLiteral("extract"));
            if (call.first().toString() == QStringLiteral("ls")) {
                listed = true;
                QCOMPARE(call.size(), 5);
                QCOMPARE(call.at(2).toString(), QStringLiteral("--path"));
                QCOMPARE(call.at(3).toString(), QStringLiteral("/"));
                QCOMPARE(call.at(4).toString(), QStringLiteral("--children-only"));
            }
        }
        QVERIFY(listed);
    }

    void captureDocumentationScreenshots()
    {
        const QString directory = qEnvironmentVariable("DWARFSCTL_SCREENSHOT_DIR");
        if (directory.isEmpty())
            QSKIP("Documentation capture was not requested");
        const QString image = qEnvironmentVariable("DWARFSCTL_SCREENSHOT_IMAGE");
        QVERIFY(QFileInfo::exists(image));
        DwarfsManagerWindow window;
        window.resize(1100, 760);
        window.openImages({image});
        window.show();
        QTest::qWait(3000);
        QVERIFY(!window.findChild<KMessageWidget *>(QStringLiteral("inlineError"))->isVisible());
        auto *rings = static_cast<ContentRingsWidget *>(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        QVERIFY(rings);
        QCOMPARE(rings->topLevelCount(), 3);
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(views);
        views->setCurrentIndex(0);
        QTest::qWait(200);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-overview.png")));
        views->setCurrentIndex(1);
        auto *browser = window.findChild<ImageBrowserWidget *>();
        QVERIFY(browser);
        QTRY_VERIFY_WITH_TIMEOUT(browser->isReady(), 10000);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-files.png")));
        window.resize(880, 600);
        QTest::qWait(200);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-files-narrow.png")));
        window.resize(1100, 760);
        views->setCurrentIndex(2);
        QTest::qWait(1000);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-contents.png")));
        window.resize(800, 620);
        QTest::qWait(200);
        QVERIFY(rings->parentWidget()->rect().contains(rings->geometry()));
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-narrow.png")));
    }
};

int main(int argc, char **argv)
{
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--mount-fixture")) {
        QCoreApplication app(argc, argv);
        auto bus = QDBusConnection::sessionBus();
        MountFixture fixture;
        if (!bus.registerVirtualObject(QString::fromLatin1(dwarfsplugin::kMountsDbusPath), &fixture)
            || !bus.registerService(QString::fromLatin1(dwarfsplugin::kTrayDbusService))) return 2;
        std::puts("ready"); std::fflush(stdout);
        return app.exec();
    }
    QApplication app(argc, argv);
    ManagerWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_manager_window.moc"
