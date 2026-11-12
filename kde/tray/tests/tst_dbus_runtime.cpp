// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the real tray on a private session bus with disposable user directories.
// Registry fixtures exercise notifications without mounting or unmounting anything.
#include <QtTest>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
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
        const auto reply = proxy.callWithArgumentList(QDBus::Block, method, args);
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
            proc.start(cli, args);
            QVERIFY(proc.waitForFinished(10000));
            QCOMPARE(proc.exitCode(), 0);
            const auto object = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
            QCOMPARE(object.value("ok"), QJsonValue(true));
        }
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
        const QJsonArray mounted{QJsonObject{{QStringLiteral("image"), image}, {QStringLiteral("mountpoint"), QStringLiteral("/proc")}}};
        registry(mounted);
        QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), 10000);
        QCOMPARE(snapshots.last(), mounted);
        QTRY_VERIFY_WITH_TIMEOUT(placesContents().contains("missing"), 10000);
        QVERIFY(!placesContents().contains("(DwarFS volume)"));
        QVERIFY(placesContents().contains("dwarfsctl-volume"));
        QVERIFY(placesContents().contains("file:///proc"));
        result = call(QStringLiteral("ListMounts"));
        envelope(result, QStringLiteral("mount_list"), true);
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounts")).toArray(), mounted);
        result = call(QStringLiteral("Status"), {image});
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounted")), QJsonValue(true));
        const auto count = snapshots.size();
        registry({});
        QTRY_VERIFY_WITH_TIMEOUT(snapshots.size() > count, 10000);
        QVERIFY(snapshots.last().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!placesContents().contains("file:///proc"), 10000);
        result = call(QStringLiteral("ListMounts"));
        QCOMPARE(result.value(QStringLiteral("details")).toObject().value(QStringLiteral("count")).toInt(), 0);
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
