// SPDX-License-Identifier: GPL-3.0-or-later
#include "trayagentjob.h"
#include "traydbus.h"
#include <QDBusConnectionInterface>

#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>
#include <KLocalizedString>
#include <cstdio>

class FakeTray final : public QDBusVirtualObject {
    Q_OBJECT
public:
    QString introspect(const QString &path) const override
    {
        if (path == QLatin1String(dwarfsplugin::kTrayDbusPath))
            return QStringLiteral("<interface name='org.quest.entropy.dwarfsctl.Tray'><method name='Ping'/></interface>");
        return QStringLiteral("<interface name='org.quest.entropy.dwarfsctl.Mounts1'>"
            "<method name='Success'><arg direction='out' type='s'/></method>"
            "<method name='Failure'><arg direction='out' type='s'/></method>"
            "<method name='Malformed'><arg direction='out' type='s'/></method>"
            "<method name='Delayed'><arg direction='out' type='s'/></method>"
            "<method name='NoReply'><arg direction='out' type='s'/></method>"
            "<method name='BusFailure'><arg direction='out' type='s'/></method></interface>");
    }
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.member() == QLatin1String("Ping") || message.member() == QLatin1String("Introspect")) {
            message.setDelayedReply(true);
            QTimer::singleShot(1000, this, [message, connection] {
                connection.send(message.createReply(message.member() == QLatin1String("Introspect")
                    ? QStringLiteral("<node/>") : QString()));
            });
            return true;
        }
        if (message.member() == QLatin1String("NoReply"))
            return connection.send(message.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.NoReply"), QStringLiteral("No reply")));
        if (message.member() == QLatin1String("BusFailure"))
            return connection.send(message.createErrorReply(QStringLiteral("org.test.Failed"), QStringLiteral("server error")));
        const QString payload = message.member() == QLatin1String("Malformed") ? QStringLiteral("not json")
            : message.member() == QLatin1String("Failure")
            ? QStringLiteral(R"({"ok":false,"command":"mount","error":{"code":4,"kind":"Mount","message":"<literal> failed"},"details":{}})")
            : QStringLiteral(R"({"ok":true,"command":"mount","details":{}})");
        if (message.member() == QLatin1String("Delayed")) {
            message.setDelayedReply(true);
            QTimer::singleShot(250, this, [message, connection, payload] {
                if (connection.send(message.createReply(payload))) {
                    std::puts("delayed-reply-sent"); std::fflush(stdout);
                }
            });
            return true;
        }
        return connection.send(message.createReply(payload));
    }
};

class TrayAgentJobTest final : public QObject {
    Q_OBJECT
    QProcess server;
private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(qEnvironmentVariable("DWARFSCTL_TEST_PRIVATE_BUS") == QLatin1String("1"), "Run on a dedicated dbus-run-session with DWARFSCTL_TEST_PRIVATE_BUS=1");
        KLocalizedString::setApplicationDomain("dwarfsctl");
        auto bus = QDBusConnection::sessionBus();
        QVERIFY(bus.isConnected());
        QVERIFY(!bus.interface()->isServiceRegistered(QString::fromLatin1(dwarfsplugin::kTrayDbusService)));
        server.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--fake-tray")});
        QVERIFY(server.waitForStarted(3000));
        QVERIFY(server.waitForReadyRead(3000));
        QCOMPARE(server.readLine().trimmed(), QByteArray("ready"));
    }
    void cleanupTestCase()
    {
        server.terminate();
        if (!server.waitForFinished(3000)) { server.kill(); server.waitForFinished(1000); }
    }
    void startDoesNotWaitForIntrospectionOrPing()
    {
        dwarfsplugin::TrayAgentJob job({QStringLiteral("mount"), QStringLiteral("Success"), {}, 1000});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        QElapsedTimer elapsed;
        elapsed.start();
        job.start();
        QVERIFY2(elapsed.elapsed() < 200, "Starting a mount job blocked the event loop");
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error(), 0);
    }
    void cannotClaimCancellationOfRemoteOperation()
    {
        dwarfsplugin::TrayAgentJob job({QStringLiteral("mount"), QStringLiteral("Delayed"), {}, 1000});
        job.setAutoDelete(false);
        QVERIFY(!job.capabilities().testFlag(KJob::Killable));
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QVERIFY(!job.kill(KJob::EmitResult));
        QCOMPARE(result.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error(), 0);
        QVERIFY(job.envelope().ok);
    }
    void failuresAreNeverCancellation_data()
    {
        QTest::addColumn<QString>("method");
        QTest::newRow("backend") << QStringLiteral("Failure");
        QTest::newRow("protocol") << QStringLiteral("Malformed");
        QTest::newRow("transport") << QStringLiteral("BusFailure");
    }
    void failuresAreNeverCancellation()
    {
        QFETCH(QString, method);
        dwarfsplugin::TrayAgentJob job({QStringLiteral("mount"), method, {}, 1000});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error(), int(KJob::UserDefinedError));
        QVERIFY(job.error() != KJob::KilledJobError);
        QVERIFY(!job.envelope().ok);
        QVERIFY(!job.errorText().isEmpty());
    }
    void transportTimeoutAlsoReportsUnknownOutcome()
    {
        dwarfsplugin::TrayAgentJob job({QStringLiteral("mount"), QStringLiteral("NoReply"), {}, 1000});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error(), int(KJob::UserDefinedError));
        QCOMPARE(job.envelope().error.kind, QStringLiteral("timeout"));
        QVERIFY(job.errorText().contains(QStringLiteral("may still complete")));
    }
    void timeoutIsUnknownAndLateReplyCannotFinishAgain()
    {
        server.readAllStandardOutput();
        dwarfsplugin::TrayAgentJob job({QStringLiteral("mount"), QStringLiteral("Delayed"), {}, 30});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error(), int(KJob::UserDefinedError));
        QCOMPARE(job.envelope().error.kind, QStringLiteral("timeout"));
        QVERIFY(job.errorText().contains(QStringLiteral("may still complete")));
        QVERIFY(job.errorText().contains(QStringLiteral("mount state")));
        QTest::qWait(400);
        QVERIFY(server.readAllStandardOutput().contains("delayed-reply-sent"));
        QCOMPARE(result.count(), 1);
        QVERIFY(!job.envelope().ok);
        QCOMPARE(job.envelope().error.kind, QStringLiteral("timeout"));
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--fake-tray"))) {
        auto bus = QDBusConnection::sessionBus();
        FakeTray tray;
        if (!bus.registerService(QString::fromLatin1(dwarfsplugin::kTrayDbusService))
            || !bus.registerVirtualObject(QString::fromLatin1(dwarfsplugin::kTrayDbusPath), &tray)
            || !bus.registerVirtualObject(QString::fromLatin1(dwarfsplugin::kMountsDbusPath), &tray)) return 2;
        std::puts("ready"); std::fflush(stdout);
        return app.exec();
    }
    TrayAgentJobTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_trayagentjob.moc"
