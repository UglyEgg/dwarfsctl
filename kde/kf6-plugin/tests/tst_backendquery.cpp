// SPDX-License-Identifier: GPL-3.0-or-later
#include "backendquery.h"
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
using namespace dwarfsplugin;

class BackendQueryTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void busyReadIntentIsReplacedAndCancelled()
    {
        QObject context;
        BackendReadRetry retry(&context);
        Envelope busy;
        busy.error.kind = QStringLiteral("busy");
        int stale = 0, latest = 0;
        QVERIFY(retry.schedule(busy, [&] { ++stale; }));
        QVERIFY(retry.schedule(busy, [&] { ++latest; }));
        QTRY_COMPARE_WITH_TIMEOUT(latest, 1, 1000);
        QCOMPARE(stale, 0);
        QVERIFY(retry.schedule(busy, [&] { ++stale; }));
        retry.cancel();
        QTest::qWait(150);
        QCOMPARE(stale, 0);
        Envelope ambiguous;
        ambiguous.error.kind = QStringLiteral("timeout");
        QVERIFY(!retry.schedule(ambiguous, [&] { ++stale; }));
    }

    void busyReadBudgetEnds()
    {
        QObject context;
        BackendReadRetry retry(&context);
        Envelope busy; busy.error.kind = QStringLiteral("busy");
        bool ended = false;
        int attempts = 0;
        std::function<void()> next;
        next = [&] { ++attempts; if (!retry.schedule(busy, next)) ended = true; };
        next();
        QTRY_VERIFY_WITH_TIMEOUT(ended, 6500);
        QVERIFY(attempts > 1);
        QVERIFY(attempts <= 55);
    }

    void freshSettingsRecoveryAndResponsiveLifetime()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile backend(directory.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"PY(#!/usr/bin/python3
import json,sys,time
from pathlib import Path
root=Path(sys.argv[0]).parent
assert sys.argv[1:]==['config','show','--section','ui']
mode=(root/'mode').read_text()
if mode=='slow': time.sleep(.25)
if mode=='hang': time.sleep(30)
if mode=='invalid':
 print(json.dumps(dict(ok=True,details={}))); sys.exit()
print(json.dumps(dict(ok=True,details=dict(command_timeout_ms=0,build_timeout_ms=1200,open_after_mount=mode!='false'))))
)PY");
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        const auto restore = qScopeGuard([&] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        auto mode = [&](const QByteArray &value) { QFile file(directory.filePath(QStringLiteral("mode"))); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(value) == value.size(); };
        QObject context;
        bool done = false, valid = false;
        dwarfsctl_kde::UiConfig settings;
        auto read = [&] { done = false; BackendQuery::ui(&context, [&](bool ok, const dwarfsctl_kde::UiConfig &config, const QString &) { done = true; valid = ok; settings = config; }); };
        QVERIFY(mode("slow"));
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++ticks; }); timer.start(10);
        read();
        QVERIFY(!done);
        QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
        QVERIFY(valid); QVERIFY(ticks >= 5); QVERIFY(settings.from_backend);
        QCOMPARE(settings.command_timeout_ms, -1); QCOMPARE(settings.build_timeout_ms, 1200);
        QVERIFY(mode("invalid")); read(); QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QVERIFY(!valid);
        QVERIFY(mode("false")); read(); QTRY_VERIFY_WITH_TIMEOUT(done, 3000); QVERIFY(valid); QVERIFY(!settings.open_after_mount);
        // A disposed surface must not be called back after owned process cleanup.
        QVERIFY(mode("hang"));
        auto *disposed = new QObject;
        bool called = false;
        BackendQuery::ui(disposed, [&](bool, const dwarfsctl_kde::UiConfig &, const QString &) { called = true; });
        QTest::qWait(100);
        QElapsedTimer elapsed; elapsed.start(); delete disposed;
        QVERIFY(elapsed.elapsed() < 100);
        QTest::qWait(150);
        QVERIFY(!called);
    }
};
QTEST_MAIN(BackendQueryTest)
#include "tst_backendquery.moc"
