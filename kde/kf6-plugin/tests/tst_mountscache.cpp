// SPDX-License-Identifier: GPL-3.0-or-later
#include "mountscache.h"
#include "traydbus.h"
#include <QDBusConnectionInterface>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QtTest>
#include <cstdio>

static const QString populated = QStringLiteral(R"([{"image":"/image.dwarfs","mountpoint":"/volume"}])");
static QString envelope(const QString &array)
{
    return QStringLiteral("{\"ok\":true,\"details\":{\"mounts\":%1}}").arg(array);
}

class FakeMounts final : public QDBusVirtualObject {
    Q_OBJECT
public:
    QString mode;
    int calls = 0;
    QString introspect(const QString &path) const override
    {
        if (path == QLatin1String(dwarfsplugin::kTrayDbusPath))
            return QStringLiteral("<interface name='org.quest.entropy.dwarfsctl.Tray'><method name='Ping'/></interface>");
        return QStringLiteral("<interface name='org.quest.entropy.dwarfsctl.Mounts1'>"
            "<method name='ListMounts'><arg direction='out' type='s'/></method>"
            "<method name='Emit'><arg direction='in' type='s'/></method>"
            "<method name='Relinquish'/><method name='Calls'><arg direction='out' type='i'/></method>"
            "<signal name='MountsChanged'><arg type='s'/></signal></interface>");
    }
    bool handleMessage(const QDBusMessage &msg, const QDBusConnection &bus) override
    {
        if (msg.member() == QLatin1String("Ping")) {
            msg.setDelayedReply(true);
            QTimer::singleShot(1500, this, [msg, bus] { bus.send(msg.createReply()); });
            return true;
        }
        if (msg.member() == QLatin1String("Calls")) return bus.send(msg.createReply(calls));
        if (msg.member() == QLatin1String("Relinquish")) {
            auto connection = bus;
            connection.unregisterService(QString::fromLatin1(dwarfsplugin::kTrayDbusService));
            return bus.send(msg.createReply());
        }
        if (msg.member() == QLatin1String("Emit")) {
            auto signal = QDBusMessage::createSignal(QString::fromLatin1(dwarfsplugin::kMountsDbusPath),
                QString::fromLatin1(dwarfsplugin::kMountsDbusInterface), QStringLiteral("MountsChanged"));
            signal << msg.arguments().first();
            bus.send(signal);
            return bus.send(msg.createReply());
        }
        if (msg.member() != QLatin1String("ListMounts")) return false;
        ++calls;
        if (mode == QLatin1String("fail") || (mode == QLatin1String("fail-once") && calls == 1)
            || (mode == QLatin1String("fail-three") && calls <= 3))
            return bus.send(msg.createErrorReply(QStringLiteral("org.test.Unavailable"), QStringLiteral("Unavailable")));
        QString payload = envelope(mode == QLatin1String("empty") ? QStringLiteral("[]") : populated);
        if (mode == QLatin1String("malformed")) payload = envelope(QStringLiteral("[{},false]"));
        if (mode == QLatin1String("stale")) {
            msg.setDelayedReply(true);
            QTimer::singleShot(500, this, [msg, bus, payload] { bus.send(msg.createReply(payload)); });
            return true;
        }
        return bus.send(msg.createReply(payload));
    }
};

class MountsCacheTest final : public QObject {
    Q_OBJECT
    QProcess server;
    dwarfsplugin::MountsCache *cache = nullptr;
    QString owner;
    void start(const QString &mode)
    {
        server.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--server"), mode});
        QVERIFY(server.waitForStarted(3000));
        QVERIFY(server.waitForReadyRead(3000));
        QCOMPARE(server.readLine().trimmed(), QByteArray("ready"));
        auto reply = QDBusConnection::sessionBus().interface()->serviceOwner(QString::fromLatin1(dwarfsplugin::kTrayDbusService));
        QVERIFY(reply.isValid());
        owner = reply.value();
        QElapsedTimer startup;
        startup.start();
        cache->ensureStarted();
        QVERIFY2(startup.elapsed() < 100, qPrintable(QStringLiteral("Cache startup blocked %1 ms").arg(startup.elapsed())));
    }
    QDBusMessage control(const QString &method, const QVariantList &args = {})
    {
        auto msg = QDBusMessage::createMethodCall(owner, QString::fromLatin1(dwarfsplugin::kMountsDbusPath),
            QString::fromLatin1(dwarfsplugin::kMountsDbusInterface), method);
        msg.setArguments(args);
        return QDBusConnection::sessionBus().call(msg, QDBus::Block, 1000);
    }
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(qEnvironmentVariable("DWARFSCTL_TEST_PRIVATE_BUS") == QLatin1String("1"), "Requires a dedicated dbus-run-session");
        QVERIFY(!QDBusConnection::sessionBus().interface()->isServiceRegistered(QString::fromLatin1(dwarfsplugin::kTrayDbusService)));
        cache = dwarfsplugin::MountsCache::instance();
        QVERIFY(!cache->isKnown());
    }
    void cleanup()
    {
        server.terminate();
        if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(1000); }
        QTRY_VERIFY(!cache->isKnown());
    }
    void failedInitialSnapshotRecoversWithoutSignal()
    {
        start(QStringLiteral("fail-once"));
        QSignalSpy changed(cache, &dwarfsplugin::MountsCache::mountsChanged);
        QTRY_VERIFY_WITH_TIMEOUT(cache->isKnown(), 3000);
        QVERIFY(cache->hasImage(QStringLiteral("/image.dwarfs")));
        QCOMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 2);
        QCOMPARE(changed.count(), 1);
    }
    void replacementWithEmptySnapshot()
    {
        QVERIFY(!cache->hasImage(QStringLiteral("/image.dwarfs"))); // unavailable rows are not live state
        start(QStringLiteral("empty"));
        QTRY_VERIFY(cache->isKnown());
        QVERIFY(cache->mounts().isEmpty());
    }
    void replacementWithNonemptySnapshot()
    {
        start(QStringLiteral("normal"));
        QTRY_VERIFY(cache->isKnown());
        QCOMPARE(cache->mountpointForImage(QStringLiteral("/image.dwarfs")), QStringLiteral("/volume"));
    }
    void malformedSnapshotIsUnknownAndRetriesAreBounded()
    {
        start(QStringLiteral("malformed"));
        QTRY_COMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 3);
        QVERIFY(!cache->isKnown());
        QVERIFY(!cache->hasImage(QStringLiteral("/image.dwarfs")));
        for (int i = 0; i < 100; ++i) cache->ensureStarted();
        QTest::qWait(800);
        QCOMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 3);
        QVERIFY(!cache->isKnown());
    }
    void exhaustedRetriesRecoverOnLaterDemand()
    {
        start(QStringLiteral("fail-three"));
        QTRY_COMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 3);
        QVERIFY(!cache->isKnown());
        QTest::qWait(5100);
        cache->ensureStarted();
        QTRY_VERIFY(cache->isKnown());
        QCOMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 4);
    }
    void signalSupersedesPendingSeed()
    {
        start(QStringLiteral("stale"));
        QTRY_COMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 1);
        control(QStringLiteral("Emit"), {QStringLiteral("[]")});
        QTRY_VERIFY(cache->isKnown());
        QVERIFY(cache->mounts().isEmpty());
        QTest::qWait(650);
        QVERIFY(cache->isKnown());
        QVERIFY(cache->mounts().isEmpty());
    }
    void oldOwnerReplyAndSignalCannotReplaceNewOwner()
    {
        start(QStringLiteral("stale"));
        QTRY_COMPARE(control(QStringLiteral("Calls")).arguments().first().toInt(), 1);
        control(QStringLiteral("Relinquish"));
        QTRY_VERIFY(!cache->isKnown());
        const auto oldOwner = owner;
        QProcess newOwner;
        newOwner.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--server"), QStringLiteral("empty")});
        QVERIFY(newOwner.waitForStarted(3000));
        QVERIFY(newOwner.waitForReadyRead(3000));
        QCOMPARE(newOwner.readLine().trimmed(), QByteArray("ready"));
        QTRY_VERIFY(cache->isKnown());
        QVERIFY(cache->mounts().isEmpty());
        owner = oldOwner;
        control(QStringLiteral("Emit"), {populated});
        QTest::qWait(650);
        QVERIFY(cache->isKnown());
        QVERIFY(cache->mounts().isEmpty());
        newOwner.terminate();
        QVERIFY(newOwner.waitForFinished(3000));
    }
    void malformedSignalInvalidatesAndReseeds()
    {
        start(QStringLiteral("normal"));
        QTRY_VERIFY(cache->isKnown());
        QSignalSpy changed(cache, &dwarfsplugin::MountsCache::mountsChanged);
        control(QStringLiteral("Emit"), {QStringLiteral("[{}]")});
        QTRY_VERIFY(changed.count() >= 2);
        QVERIFY(cache->isKnown());
        QVERIFY(cache->hasImage(QStringLiteral("/image.dwarfs")));
    }
};

int main(int argc, char **argv)
{
    // Isolate service activation and all application state as well as the bus.
    QTemporaryDir state;
    if (!state.isValid()) return 2;
    for (const auto *name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR", "XDG_STATE_HOME"}) {
        const auto path = state.path() + QLatin1Char('/') + QLatin1String(name);
        if (!QDir().mkpath(path)) return 2;
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv(name, path.toUtf8());
    }
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--server"))) {
        auto bus = QDBusConnection::sessionBus();
        FakeMounts object;
        object.mode = app.arguments().last();
        if (!bus.registerVirtualObject(QString::fromLatin1(dwarfsplugin::kMountsDbusPath), &object)
            || !bus.registerVirtualObject(QString::fromLatin1(dwarfsplugin::kTrayDbusPath), &object)
            || !bus.registerService(QString::fromLatin1(dwarfsplugin::kTrayDbusService))) return 2;
        std::puts("ready"); std::fflush(stdout);
        return app.exec();
    }
    MountsCacheTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_mountscache.moc"
