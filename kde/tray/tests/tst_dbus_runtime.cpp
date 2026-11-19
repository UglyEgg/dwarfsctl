// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the real tray on a private session bus with disposable user directories.
// Registry fixtures exercise notifications without mounting or unmounting anything.
#include <QtTest>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusPendingReply>
#include <QLockFile>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>

static QString trayBinary;
static const QString service = QStringLiteral("org.quest.entropy.dwarfsctl.Tray");
static const QString path = QStringLiteral("/org/quest/entropy/dwarfsctl/Mounts");
static const QString interface = QStringLiteral("org.quest.entropy.dwarfsctl.Mounts1");

class RuntimeContract : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    QProcess tray;
    QList<QJsonArray> snapshots;

    QJsonObject call(const QString &method, const QVariantList &args = {}) {
        QDBusInterface proxy(service, path, interface, QDBusConnection::sessionBus());
        proxy.setTimeout(5000);
        auto reply = proxy.callWithArgumentList(QDBus::Block, method, args);
        QElapsedTimer timer; timer.start();
        while (reply.type() != QDBusMessage::ErrorMessage && reply.arguments().size() == 1
            && QJsonDocument::fromJson(reply.arguments().first().toString().toUtf8()).object()
                .value(QStringLiteral("error")).toObject().value(QStringLiteral("kind")).toString() == QLatin1String("Busy")
            && timer.elapsed() < 5000) {
            QTest::qWait(20);
            reply = proxy.callWithArgumentList(QDBus::Block, method, args);
        }
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().size() != 1) {
            QTest::qFail(qPrintable(reply.errorMessage()), __FILE__, __LINE__);
            return {};
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(reply.arguments().first().toString().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            QTest::qFail("method did not return a JSON object", __FILE__, __LINE__);
            return {};
        }
        return doc.object();
    }

    void envelope(const QJsonObject &result, const QString &command, bool ok) {
        QCOMPARE(result.value(QStringLiteral("ok")), QJsonValue(ok));
        QCOMPARE(result.value(QStringLiteral("command")).toString(), command);
        QVERIFY(result.contains(QStringLiteral("image")));
        QVERIFY(result.contains(QStringLiteral("details")));
        QVERIFY(result.value(QStringLiteral("warnings")).isArray());
        QVERIFY(result.value(QStringLiteral("warning_objects")).isArray());
        if (ok) {
            QVERIFY(result.value(QStringLiteral("error")).isNull());
        } else {
            const auto error = result.value(QStringLiteral("error")).toObject();
            QVERIFY(error.value(QStringLiteral("code")).isDouble());
            QVERIFY(!error.value(QStringLiteral("kind")).toString().isEmpty());
            QVERIFY(!error.value(QStringLiteral("message")).toString().isEmpty());
        }
    }

    void registry(const QJsonArray &mounts) {
        QSaveFile file(home.filePath(QStringLiteral("runtime/dwarfsctl/mounts.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write(QJsonDocument(QJsonObject{{QStringLiteral("mounts"), mounts}}).toJson()) > 0);
        QVERIFY(file.commit());
    }

    QByteArray placesContents() const {
        QFile file(home.filePath(QStringLiteral("user-places.xbel")));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }

private Q_SLOTS:
    void changed(const QString &json) {
        const auto doc = QJsonDocument::fromJson(json.toUtf8());
        QVERIFY(doc.isArray());
        snapshots.append(doc.array());
    }

    void initTestCase() {
        QVERIFY(home.isValid());
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        // Refuse to share a bus with an existing tray; CTest uses dbus-run-session.
        QVERIFY(!bus.interface()->isServiceRegistered(service).value());
        const QString runtime = home.filePath(QStringLiteral("runtime"));
        QVERIFY(QDir().mkpath(runtime + QStringLiteral("/dwarfsctl")));
        QVERIFY(QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto &name : {"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"})
            env.insert(QString::fromLatin1(name), home.path());
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        tray.setProcessEnvironment(env);
        tray.start(trayBinary, {});
        QVERIFY(tray.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(bus.interface()->isServiceRegistered(service).value(), 10000);
        QVERIFY(bus.connect(service, path, interface, QStringLiteral("MountsChanged"), this, SLOT(changed(QString))));
    }

    void cliForwarding() {
        const QString cli = qEnvironmentVariable("DWARFSCTL_BIN");
        if (cli.isEmpty()) QSKIP("Set DWARFSCTL_BIN to test the public CLI client");
        for (const QStringList args : {QStringList{"mount", "list"}, QStringList{"mount", "status", home.filePath("absent.dwarfs")}, QStringList{"unmount", "all"}, QStringList{"unmount", "last"}}) {
            QProcess proc;
            proc.setProcessEnvironment(tray.processEnvironment());
            QElapsedTimer readiness; readiness.start();
            QJsonObject object;
            for (;;) {
                proc.start(cli, args);
                QVERIFY(proc.waitForFinished(10000));
                object = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
                // Service ownership precedes its initial refresh, and a
                // mutation schedules another refresh. Busy explicitly rejects
                // dispatch; timeouts and other uncertain outcomes are not retried.
                if (proc.exitCode() != 1 || object.value(QStringLiteral("error")).toObject()
                        .value(QStringLiteral("kind")).toString() != QLatin1String("Busy")
                    || readiness.elapsed() >= 5000) break;
                QTest::qWait(20);
            }
            QCOMPARE(proc.exitCode(), 0);
            QCOMPARE(object.value("ok"), QJsonValue(true));
        }
    }

    void pendingRegistryWorkKeepsDbusResponsive()
    {
        // A genuine held registry lock keeps the executor busy without any
        // mount, engine helper, or instrumentation in the service.
        QVERIFY(call(QStringLiteral("ListMounts")).value(QStringLiteral("ok")).toBool());
        QTest::qWait(100);
        QLockFile lock(home.filePath(QStringLiteral("runtime/dwarfsctl/mounts.lock")));
        bool acquired = false;
        QTRY_VERIFY_WITH_TIMEOUT((acquired = acquired || lock.tryLock()), 7000);
        QDBusInterface proxy(service, path, interface, QDBusConnection::sessionBus());
        proxy.setTimeout(7000);
        QDBusPendingReply<QString> pending = proxy.asyncCall(QStringLiteral("ListMounts"));
        QTest::qWait(100);
        QElapsedTimer timer; timer.start();
        QDBusReply<int> version = proxy.call(QStringLiteral("ApiMajor"));
        QVERIFY(version.isValid());
        QCOMPARE(version.value(), 1);
        QVERIFY2(timer.elapsed() < 1000, "Mount authority blocked the D-Bus event loop");
        const auto reply = proxy.call(QStringLiteral("Status"), QStringLiteral("/absent.dwarfs"));
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
        const auto busy = QJsonDocument::fromJson(reply.arguments().first().toString().toUtf8()).object();
        QCOMPARE(busy.value(QStringLiteral("error")).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("Busy"));
        lock.unlock();
        QTRY_VERIFY_WITH_TIMEOUT(pending.isFinished(), 7000);
        QVERIFY(!pending.isError());
    }

    void methodsAndSignals() {
        auto bus = QDBusConnection::sessionBus();
        QDBusInterface proxy(service, path, interface, bus);
        QDBusReply<int> version = proxy.call(QStringLiteral("ApiMajor"));
        QVERIFY(version.isValid());
        QCOMPARE(version.value(), 1);
        QDBusInterface introspect(service, path, QStringLiteral("org.freedesktop.DBus.Introspectable"), bus);
        QDBusReply<QString> xml = introspect.call(QStringLiteral("Introspect"));
        QVERIFY(xml.isValid());
        for (const auto &method : {"ApiMajor", "ListMounts", "Status", "Mount", "UnmountByMountpoint", "UnmountByImage", "UnmountAll", "UnmountLast"})
            QVERIFY2(xml.value().contains(QStringLiteral("name=\"%1\"").arg(QString::fromLatin1(method))), method);
        QVERIFY(xml.value().contains(QStringLiteral("name=\"MountsChanged\"")));

        auto result = call(QStringLiteral("ListMounts"));
        envelope(result, QStringLiteral("mount_list"), true);
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("count")).toInt(), 0);
        const QString image = home.filePath(QStringLiteral("missing.dwarfs"));
        result = call(QStringLiteral("Status"), {image});
        envelope(result, QStringLiteral("mount_status"), true);
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounted")), QJsonValue(false));
        envelope(call(QStringLiteral("Mount"), {image}), QStringLiteral("mount"), false);
        envelope(call(QStringLiteral("UnmountAll")), QStringLiteral("unmount_all"), true);
        envelope(call(QStringLiteral("UnmountLast")), QStringLiteral("unmount_last"), true);

        // /proc is already a readable mount. Only the isolated registry is changed;
        // never call an unmount method while this fixture is present.
        // Match the kernel's actual source name for this read-only reference.
        // This is notification/serialization evidence, not a DwarFS mount.
        const QString referenceSource = QStringLiteral("proc");
        const QJsonArray mounted{QJsonObject{{QStringLiteral("image"), referenceSource}, {QStringLiteral("mountpoint"), QStringLiteral("/proc")}}};
        registry(mounted);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), 10000);
        QCOMPARE(snapshots.last(), mounted);
        QTRY_VERIFY_WITH_TIMEOUT(placesContents().contains("proc"), 10000);
        QVERIFY(!placesContents().contains("(DwarFS volume)"));
        QVERIFY(placesContents().contains("dwarfsctl-volume"));
        QVERIFY(placesContents().contains("file:///proc"));
        result = call(QStringLiteral("ListMounts"));
        envelope(result, QStringLiteral("mount_list"), true);
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounts")).toArray(), mounted);
        result = call(QStringLiteral("Status"), {referenceSource});
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounted")), QJsonValue(true));
        const auto count = snapshots.size();
        registry({});
        QTRY_VERIFY_WITH_TIMEOUT(snapshots.size() > count, 10000);
        QVERIFY(snapshots.last().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!placesContents().contains("file:///proc"), 10000);
        result = call(QStringLiteral("ListMounts"));
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("count")).toInt(), 0);
    }

    void registryRotationDeletionAndRecreation()
    {
        const QJsonArray mounted{QJsonObject{{QStringLiteral("image"), QStringLiteral("proc")},
            {QStringLiteral("mountpoint"), QStringLiteral("/proc")}}};
        registry(mounted);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty() && snapshots.last() == mounted, 10000);
        QVERIFY(QFile::remove(home.filePath(QStringLiteral("runtime/dwarfsctl/mounts.json"))));
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty() && snapshots.last().isEmpty(), 10000);
        registry(mounted);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty() && snapshots.last() == mounted, 10000);
        registry({});
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty() && snapshots.last().isEmpty(), 10000);
    }

    void idleAuthorityDoesNotRescheduleForItsOwnLocks()
    {
        QVERIFY(call(QStringLiteral("ListMounts")).value(QStringLiteral("ok")).toBool());
        QTest::qWait(200);
        QDBusInterface proxy(service, path, interface, QDBusConnection::sessionBus());
        for (int i = 0; i < 20; ++i) {
            const auto reply = proxy.call(QStringLiteral("Status"), QStringLiteral("/absent.dwarfs"));
            QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
            const auto value = QJsonDocument::fromJson(reply.arguments().first().toString().toUtf8()).object();
            QVERIFY2(value.value(QStringLiteral("ok")).toBool(), "An idle authority was starved by its own refresh notifications");
            QTest::qWait(10);
        }
    }

    void cleanupTestCase() {
        tray.terminate();
        if (!tray.waitForFinished(5000)) { tray.kill(); tray.waitForFinished(5000); }
        if (QTest::currentTestFailed()) qWarning().noquote() << tray.readAllStandardError();
    }
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    trayBinary = QString::fromLocal8Bit(argv[1]);
    int qtArgc = 1;
    QCoreApplication app(qtArgc, argv);
    RuntimeContract test;
    return QTest::qExec(&test, qtArgc, argv);
}
#include "tst_dbus_runtime.moc"
