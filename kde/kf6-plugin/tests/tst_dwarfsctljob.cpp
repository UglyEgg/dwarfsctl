// SPDX-License-Identifier: GPL-3.0-or-later
#include "dwarfsctljob.h"
#include <QFile>
#include <QProcess>
#include <QPointer>
#include <QScopeGuard>
#include <KLocalizedString>
#include <signal.h>
#include <QTemporaryDir>
#include <QtTest>
using namespace dwarfsplugin;
class JobTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void responseFormatsAndLimits_data()
    {
        QTest::addColumn<QByteArray>("stdoutBytes");
        QTest::addColumn<QByteArray>("stderrBytes");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<bool>("text");
        QTest::addColumn<qint64>("limit");
        QTest::addColumn<bool>("success");
        const QByteArray json = R"({"ok":true,"details":{}})";
        QTest::newRow("plain version") << QByteArray("dwarfsctl 1.2.3\n") << QByteArray() << 0 << true << qint64(1024) << true;
        QTest::newRow("plain failure") << QByteArray("partial") << QByteArray("writer failed") << 9 << true << qint64(1024) << false;
        QTest::newRow("json success") << json << QByteArray() << 0 << false << qint64(1024) << true;
        QTest::newRow("stdout cap") << QByteArray(8192, 'x') << QByteArray() << 0 << true << qint64(128) << false;
        QTest::newRow("combined channels cap") << QByteArray(80, 'x') << QByteArray(80, 'y') << 0 << true << qint64(128) << false;
        QTest::newRow("valid prefix does not hide overflow") << json + QByteArray(8192, ' ') << QByteArray() << 0 << false << qint64(128) << false;
        QTest::newRow("uncapped structured data") << QByteArray("{\"ok\":true,\"details\":{\"data\":\"") + QByteArray(2 * 1024 * 1024, 'a') + QByteArray("\"}}") << QByteArray() << 0 << false << qint64(0) << true;
        QTest::newRow("uncapped structured payload") << json + QByteArray(1024 * 1024, ' ') << QByteArray() << 0 << false << qint64(0) << true;
    }
    void responseFormatsAndLimits()
    {
        QFETCH(QByteArray, stdoutBytes);
        QFETCH(QByteArray, stderrBytes);
        QFETCH(int, exitCode);
        QFETCH(bool, text);
        QFETCH(qint64, limit);
        QFETCH(bool, success);
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport sys\nsys.stdout.buffer.write(bytes.fromhex('" + stdoutBytes.toHex()
            + "'))\nsys.stdout.flush()\nsys.stderr.buffer.write(bytes.fromhex('" + stderrBytes.toHex()
            + "'))\nsys.stderr.flush()\nsys.exit(" + QByteArray::number(exitCode) + ")\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob::Request request{QString(), {}, 3000, false};
        request.outputFormat = text ? DwarfsctlJob::OutputFormat::Text : DwarfsctlJob::OutputFormat::Json;
        request.responseLimit = limit;
        DwarfsctlJob job(request);
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QCOMPARE(job.error() == 0, success);
        QCOMPARE(job.envelope().ok, success);
        if (success) QCOMPARE(job.textOutput(), QString::fromUtf8(stdoutBytes));
        if (limit > 0 && stdoutBytes.size() + stderrBytes.size() > limit) {
            QCOMPARE(job.envelope().error.kind, QStringLiteral("protocol"));
            QVERIFY(job.envelope().details.value(QStringLiteral("response_truncated")).toBool());
            QVERIFY(job.textOutput().size() <= limit);
        }
        QVERIFY(job.error() != KJob::KilledJobError);
    }
    void progressConsumerDestructionStopsOwnedProcess_data()
    {
        QTest::addColumn<bool>("destroyContext");
        QTest::newRow("delete job") << false;
        QTest::newRow("delete consumer context") << true;
    }
    void progressConsumerDestructionStopsOwnedProcess()
    {
        QFETCH(bool, destroyContext);
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        const auto record = directory.filePath(QStringLiteral("worker"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import os,signal,sys,time
signal.signal(signal.SIGTERM,signal.SIG_IGN)
with open(sys.argv[1],'w') as record: record.write(str(os.getpid()))
os.write(2,b'{"event":"build_progress","phase":"working"}\n')
while True: time.sleep(1)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        QPointer<QObject> context = new QObject;
        DwarfsctlJob::Request request{QString(), {record}, 0, false};
        request.responseLimit = 1024 * 1024;
        QPointer<DwarfsctlJob> job = new DwarfsctlJob(request, context);
        QSignalSpy result(job, &KJob::result);
        bool consumed = false;
        connect(job, &DwarfsctlJob::buildPhase, this, [&] {
            consumed = true;
            if (destroyContext) delete context;
            else delete job;
        });
        job->start();
        QTRY_VERIFY_WITH_TIMEOUT(consumed, 3000);
        QVERIFY(!job);
        QFile worker(record);
        QVERIFY(worker.open(QIODevice::ReadOnly));
        const auto pid = worker.readAll().toLongLong();
        QVERIFY(pid > 0);
        QFile initialStat(QStringLiteral("/proc/%1/stat").arg(pid));
        QVERIFY(initialStat.open(QIODevice::ReadOnly));
        const auto initialBytes = initialStat.readAll();
        const auto identity = initialBytes.mid(initialBytes.lastIndexOf(')') + 2).split(' ').value(19);
        const auto alive = [pid, identity] {
            QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
            if (!stat.open(QIODevice::ReadOnly)) return false;
            const auto bytes = stat.readAll();
            const auto fields = bytes.mid(bytes.lastIndexOf(')') + 2).split(' ');
            return fields.value(19) == identity && fields.value(0) != "Z" && fields.value(0) != "X";
        };
        const auto cleanup = qScopeGuard([&] { if (alive()) ::kill(pid, SIGKILL); delete context; });
        QCOMPARE(result.count(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(), 5000);
        QCOMPARE(result.count(), 0);
    }
    void admissionBoundsProcessesAndRecovers()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        const auto records = directory.filePath(QStringLiteral("starts"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport os,signal,time\nwith open('" + records.toUtf8()
                     + "','a') as f: f.write(str(os.getpid())+'\\n')\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\ntime.sleep(30)\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        QList<DwarfsctlJob *> active;
        const auto cleanup = qScopeGuard([&] { qDeleteAll(active); });
        auto launch = [&](QStringList args) {
            auto *job = new DwarfsctlJob({QString(), args, 0, false});
            job->setAutoDelete(false);
            active.append(job);
            job->start();
            return job;
        };
        auto *first = launch({QStringLiteral("ls"), directory.filePath(QStringLiteral("image")), QStringLiteral("--path"), QStringLiteral("/")});
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(records), 3000);
        for (int retry = 0; retry < 20; ++retry) {
            auto *duplicate = launch({QStringLiteral("--config"), directory.filePath(QStringLiteral("config")),
                                     QStringLiteral("ls"), directory.filePath(QStringLiteral("./image")),
                                     QStringLiteral("--path"), QString::number(retry)});
            QSignalSpy rejected(duplicate, &KJob::result);
            QTRY_COMPARE_WITH_TIMEOUT(rejected.count(), 1, 1000);
            QCOMPARE(duplicate->envelope().error.kind, QStringLiteral("busy"));
            QVERIFY(duplicate->error() != KJob::KilledJobError);
        }
        // Other operations on the same image and reads of another image work.
        launch({QStringLiteral("status"), directory.filePath(QStringLiteral("image"))});
        launch({QStringLiteral("usage"), directory.filePath(QStringLiteral("image"))});
        for (int other = 0; other < 5; ++other)
            launch({QStringLiteral("peek"), directory.filePath(QStringLiteral("other%1").arg(other))});
        auto *saturated = launch({QStringLiteral("build"), QStringLiteral("source")});
        QSignalSpy saturatedResult(saturated, &KJob::result);
        QTRY_COMPARE_WITH_TIMEOUT(saturatedResult.count(), 1, 1000);
        QCOMPARE(saturated->envelope().error.kind, QStringLiteral("busy"));
        const auto starts = [&] { QFile file(records); return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0; };
        QTRY_COMPARE_WITH_TIMEOUT(starts(), 8, 3000);
        QSignalSpy firstResult(first, &KJob::result);
        QVERIFY(!first->kill(KJob::EmitResult));
        QCOMPARE(firstResult.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(firstResult.count(), 1, 5000);
        auto *recovered = launch({QStringLiteral("ls"), directory.filePath(QStringLiteral("image"))});
        QVERIFY(recovered->envelope().error.kind != QLatin1String("busy"));
        QTRY_COMPARE_WITH_TIMEOUT(starts(), 9, 3000);
        QList<QSignalSpy *> results;
        const auto clearSpies = qScopeGuard([&] { qDeleteAll(results); });
        for (auto *job : active) {
            if (job == first || job->error() != 0) continue;
            auto *spy = new QSignalSpy(job, &KJob::result);
            results.append(spy);
            job->kill(KJob::EmitResult);
        }
        for (auto *spy : results) QTRY_COMPARE_WITH_TIMEOUT(spy->count(), 1, 5000);
    }
    void failedStartsAndResultCallbacksReleaseAdmission()
    {
        QTemporaryDir directory;
        const auto original = qgetenv("DWARFSCTL_BIN");
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        qputenv("DWARFSCTL_BIN", directory.filePath(QStringLiteral("missing")).toUtf8());
        const QStringList args{QStringLiteral("status"), directory.filePath(QStringLiteral("image"))};
        for (int attempt = 0; attempt < 10; ++attempt) {
            DwarfsctlJob job({QString(), args, 0, false});
            job.setAutoDelete(false);
            QSignalSpy result(&job, &KJob::result);
            job.start();
            QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
            QCOMPARE(job.envelope().error.kind, QStringLiteral("spawn"));
        }
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' '{\"ok\":true,\"command\":\"status\",\"details\":{}}'\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        DwarfsctlJob first({QString(), args, 0, false});
        DwarfsctlJob next({QString(), args, 0, false});
        first.setAutoDelete(false);
        next.setAutoDelete(false);
        QSignalSpy result(&next, &KJob::result);
        connect(&first, &KJob::result, &next, [&] { next.start(); });
        first.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(first.error(), 0);
        QCOMPARE(next.error(), 0);
    }
    void unstartedAndRepeatedStartAreSafe()
    {
        DwarfsctlJob unstarted({QString(), {}, 0, false});
        unstarted.setAutoDelete(false);
        DwarfsctlJob cancelled({QString(), {}, 0, false});
        cancelled.setAutoDelete(false);
        QSignalSpy cancelledResult(&cancelled, &KJob::result);
        QVERIFY(!cancelled.kill(KJob::EmitResult));
        cancelled.start();
        QTRY_COMPARE(cancelledResult.count(), 1);
        QCOMPARE(cancelled.error(), int(KJob::KilledJobError));
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' '{\"ok\":true,\"command\":\"status\",\"details\":{}}'\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob job({QString(), {QStringLiteral("status"), QStringLiteral("image")}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        job.start();
        QTest::qWait(50);
        QCOMPARE(result.count(), 1);
        QCOMPARE(job.error(), 0);
    }
    void failureIsNotMisclassifiedAsUserCancellation()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString executable = directory.filePath(QStringLiteral("failed-backend"));
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nprintf '%s\\n' '{\"ok\":false,\"command\":\"build\",\"error\":{\"code\":9,\"kind\":\"Internal\",\"message\":\"writer failed\"},\"details\":{}}'\nexit 9\n");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        DwarfsctlJob job({QString(), {QStringLiteral("build")}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        QVERIFY(job.capabilities().testFlag(KJob::Killable));
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QVERIFY(job.error() >= KJob::UserDefinedError);
        QVERIFY(job.error() != KJob::KilledJobError);
        QVERIFY(!job.envelope().ok);
        QVERIFY(job.errorString().contains(QStringLiteral("writer failed")));
        qputenv("DWARFSCTL_BIN", original);
    }
    void cooperativeCancellationIsAcknowledgedAfterCleanup()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import json, signal, sys
from pathlib import Path
def cancel(signum, frame):
    print(json.dumps(dict(ok=False, command='build', details=dict(cancelled=True),
                         error=dict(code=9, kind='Internal', message='Operation cancelled'))), flush=True)
    sys.exit(9)
signal.signal(signal.SIGTERM, cancel)
Path(sys.argv[0] + '.ready').touch()
while True: signal.pause()
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob job({QString(), {QStringLiteral("build")}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(executable + QStringLiteral(".ready")), 3000);
        QVERIFY(!job.kill(KJob::EmitResult));
        QCOMPARE(result.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QCOMPARE(job.error(), int(KJob::KilledJobError));
        QCOMPARE(job.envelope().details.value(QStringLiteral("cancelled")), QJsonValue(true));
        QTest::qWait(50);
        QCOMPARE(result.count(), 1);
    }
    void timeoutRemainsAnOperationalFailure()
    {
        QTemporaryDir directory;
        const QString executable = directory.filePath(QStringLiteral("slow-backend"));
        QFile file(executable);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\nsleep 30\n");
        file.close();
        QVERIFY(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        DwarfsctlJob job({QString(), {QStringLiteral("build")}, 100, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QVERIFY(job.error() >= KJob::UserDefinedError);
        QCOMPARE(job.envelope().error.kind, QStringLiteral("timeout"));
        qputenv("DWARFSCTL_BIN", original);
    }
    void terminalResultWaitsForOwnedWorkers_data()
    {
        QTest::addColumn<bool>("timeout");
        QTest::addColumn<bool>("forkDuringStop");
        QTest::addColumn<QString>("outcome");
        QTest::newRow("cancel descendant") << false << false << QStringLiteral("cancel");
        QTest::newRow("cancel newly forked descendant") << false << true << QStringLiteral("cancel");
        QTest::newRow("timeout descendant") << true << false << QStringLiteral("timeout");
        QTest::newRow("timeout newly forked descendant") << true << true << QStringLiteral("timeout");
        QTest::newRow("successful leader with live descendant") << false << false << QStringLiteral("success");
        QTest::newRow("failed leader with live descendant") << false << false << QStringLiteral("failure");
    }
    void terminalResultWaitsForOwnedWorkers()
    {
        QFETCH(bool, timeout);
        QFETCH(bool, forkDuringStop);
        QFETCH(QString, outcome);
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("worker-backend"));
        const auto records = directory.filePath(QStringLiteral("workers"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import os,signal,subprocess,sys,time,json
record=sys.argv[1]
def remember(pid):
    raw=open('/proc/'+str(pid)+'/stat').read()
    with open(record,'a') as output: output.write(str(pid)+' '+raw[raw.rfind(')')+2:].split()[19]+'\n')
def worker():
    child=subprocess.Popen([sys.executable,'-c',"import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);print('ready',flush=True);time.sleep(30)"],stdout=subprocess.PIPE,text=True)
    assert child.stdout.readline().strip()=='ready'
    remember(child.pid)
remember(os.getpid())
if sys.argv[2]=='fork':
    def stop(sig,frame):
        worker()
        os._exit(0)
    signal.signal(signal.SIGTERM,stop)
else:
    signal.signal(signal.SIGTERM,signal.SIG_IGN)
worker()
if sys.argv[3]=='success':
    print(json.dumps(dict(ok=True,command='build',details={})))
    sys.exit(0)
if sys.argv[3]=='failure':
    print(json.dumps(dict(ok=False,command='build',details={},error=dict(code=9,kind='Internal',message='writer failed'))))
    sys.exit(9)
time.sleep(30)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        const auto workers = [&] {
            QFile file(records);
            QList<QPair<qint64, qint64>> result;
            if (file.open(QIODevice::ReadOnly)) {
                for (const auto &line : file.readAll().split('\n')) {
                    const auto parts = line.split(' ');
                    if (parts.size() == 2) result.append({parts.at(0).toLongLong(), parts.at(1).toLongLong()});
                }
            }
            return result;
        };
        const auto alive = [](const QPair<qint64,qint64> &worker) {
            QFile file(QStringLiteral("/proc/%1/stat").arg(worker.first));
            if (!file.open(QIODevice::ReadOnly)) return false;
            const auto raw = file.readAll();
            const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
            return fields.size() > 19 && fields.at(19).toLongLong() == worker.second
                && fields.at(0) != "Z" && fields.at(0) != "X";
        };
        const auto cleanup = qScopeGuard([&] { for (const auto &worker : workers()) if (alive(worker)) ::kill(worker.first, SIGKILL); });
        QProcess unrelated;
        unrelated.start(QStringLiteral("/usr/bin/python3"), {QStringLiteral("-c"), QStringLiteral("import time;time.sleep(30)")});
        QVERIFY(unrelated.waitForStarted(3000));
        const auto stopUnrelated = qScopeGuard([&] { unrelated.terminate(); if (!unrelated.waitForFinished(1000)) { unrelated.kill(); unrelated.waitForFinished(1000); } });
        DwarfsctlJob job({QString(), {records, forkDuringStop ? QStringLiteral("fork") : QStringLiteral("normal"), outcome}, timeout ? 1000 : 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        bool liveAtResult = false;
        connect(&job, &KJob::result, this, [&] { for (const auto &worker : workers()) liveAtResult |= alive(worker); });
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(workers().size(), 2, 3000);
        QElapsedTimer heartbeat;
        heartbeat.start();
        qint64 previous = heartbeat.elapsed();
        qint64 largestGap = 0;
        int beats = 0;
        QTimer pulse;
        pulse.setInterval(5);
        connect(&pulse, &QTimer::timeout, this, [&] {
            const auto now = heartbeat.elapsed();
            largestGap = qMax(largestGap, now - previous);
            previous = now;
            ++beats;
        });
        pulse.start();
        if (outcome == QLatin1String("cancel")) {
            QElapsedTimer request;
            request.start();
            QVERIFY(!job.kill(KJob::EmitResult)); // cancellation acknowledgement is pending
            QVERIFY2(request.elapsed() < 100, qPrintable(QStringLiteral("Cancel request blocked %1 ms").arg(request.elapsed())));
            QCOMPARE(result.count(), 0);
        }
        if (outcome == QLatin1String("success")) {
            QTest::qWait(100);
            QVERIFY(!job.kill(KJob::EmitResult)); // parsed completion wins cleanup cancellation
        }
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        pulse.stop();
        QVERIFY2(largestGap < 250, qPrintable(QStringLiteral("GUI heartbeat paused %1 ms").arg(largestGap)));
        QVERIFY(beats > 2);
        QCOMPARE(workers().size(), forkDuringStop ? 3 : 2);
        QVERIFY(!liveAtResult);
        QVERIFY(unrelated.state() == QProcess::Running);
        QCOMPARE(job.error() == KJob::KilledJobError, outcome == QLatin1String("cancel"));
        if (outcome == QLatin1String("success")) QCOMPARE(job.error(), 0);
        if (outcome == QLatin1String("failure")) {
            QVERIFY(job.error() >= KJob::UserDefinedError);
            const int failure = job.error();
            job.kill(KJob::EmitResult);
            QCOMPARE(job.error(), failure);
            QCOMPARE(result.count(), 1);
        }
        if (timeout) QCOMPARE(job.envelope().error.kind, QStringLiteral("timeout"));
    }
    void exitClassification_data()
    {
        QTest::addColumn<QByteArray>("output");
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<bool>("success");
        QTest::addColumn<QString>("diagnostic");
        QTest::newRow("success") << QByteArray(R"({"ok":true,"command":"status","details":{}})") << 0 << true << QString();
        QTest::newRow("structured failure") << QByteArray(R"({"ok":false,"error":{"message":"declared failure"}})") << 9 << false << QStringLiteral("declared failure");
        QTest::newRow("success with nonzero exit") << QByteArray(R"({"ok":true,"command":"status","details":{}})") << 9 << false << QStringLiteral("dwarfsctl failed");
        QTest::newRow("malformed zero") << QByteArray("not json") << 0 << false << QStringLiteral("Failed to parse");
        QTest::newRow("malformed nonzero") << QByteArray("not json") << 9 << false << QStringLiteral("dwarfsctl failed");
    }
    void exitClassification()
    {
        QFETCH(QByteArray, output);
        QFETCH(int, exitCode);
        QFETCH(bool, success);
        QFETCH(QString, diagnostic);
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' '" + output + "'\nexit " + QByteArray::number(exitCode) + "\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob job({QString(), {}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QCOMPARE(job.error() == 0, success);
        if (!success) QVERIFY2(job.errorString().contains(diagnostic), qPrintable(job.errorString()));
        QTest::qWait(100);
        QCOMPARE(result.count(), 1);
    }
    void bufferedCompletionWinsLateCancellation()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        const auto destination = directory.filePath(QStringLiteral("result"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import os,sys
with open(sys.argv[1],'wb') as result: result.write(b'complete result')
os.write(1,b'{"ok":true,"command":"build","details":{}}\n')
os.write(2,b'{"event":"build_progress","phase":"complete"}\n')
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob job({QString(), {destination}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        bool requested = false;
        connect(&job, &DwarfsctlJob::buildPhase, this, [&] {
            requested = true;
            QVERIFY(!job.kill(KJob::EmitResult));
        });
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QVERIFY(requested);
        QCOMPARE(job.error(), 0);
        QVERIFY(job.envelope().ok);
        QFile output(destination);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("complete result"));
    }
    void destructionDoesNotBlockOrAbandonOwnedProcess()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("backend"));
        const auto marker = directory.filePath(QStringLiteral("started"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/usr/bin/python3\nimport os,signal,sys,time\nsignal.signal(signal.SIGTERM,signal.SIG_IGN)\nopen(sys.argv[1],'w').write(str(os.getpid()))\ntime.sleep(30)\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        auto *job = new DwarfsctlJob({QString(), {marker}, 0, false});
        job->start();
        QTRY_VERIFY(QFile::exists(marker));
        QFile record(marker);
        QVERIFY(record.open(QIODevice::ReadOnly));
        const auto pid = record.readAll().toLongLong();
        QVERIFY(pid > 0);
        QElapsedTimer elapsed;
        elapsed.start();
        delete job;
        QVERIFY(elapsed.elapsed() < 100);
        QFile initialStat(QStringLiteral("/proc/%1/stat").arg(pid));
        QVERIFY(initialStat.open(QIODevice::ReadOnly));
        const auto initialRaw = initialStat.readAll();
        const auto identity = initialRaw.mid(initialRaw.lastIndexOf(')') + 2).split(' ').value(19);
        const auto alive = [pid, identity] {
            QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
            if (!stat.open(QIODevice::ReadOnly)) return false;
            const auto raw = stat.readAll();
            const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
            return fields.size() > 19 && fields.at(19) == identity && fields.first() != "Z" && fields.first() != "X";
        };
        const auto cleanup = qScopeGuard([&] { if (alive()) ::kill(pid, SIGKILL); });
        QTRY_VERIFY_WITH_TIMEOUT(!alive(), 5000);
    }
    void runtimeCrashIsNotAStartupFailureOrCancellation()
    {
        QTemporaryDir directory;
        const auto executable = directory.filePath(QStringLiteral("crashed-backend"));
        QFile script(executable);
        QVERIFY(script.open(QIODevice::WriteOnly));
        // Disable core dumping inside the child, without changing host policy.
        script.write(R"PY(#!/usr/bin/python3
import ctypes,os,resource,signal
library=ctypes.CDLL(None)
assert library.prctl(4,0,0,0,0)==0 and library.prctl(3,0,0,0,0)==0
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
os.kill(os.getpid(),signal.SIGSEGV)
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        DwarfsctlJob job({QString(), {QStringLiteral("fsck")}, 500, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 3000);
        QVERIFY(job.error() >= KJob::UserDefinedError);
        QVERIFY(job.error() != KJob::KilledJobError);
        QCOMPARE(job.envelope().error.kind, QStringLiteral("process"));
        QVERIFY(!job.envelope().error.message.contains(QStringLiteral("Failed to start")));
        QTest::qWait(550);
        QCOMPARE(result.count(), 1);
        QCOMPARE(job.envelope().error.kind, QStringLiteral("process"));
    }
    void missingBackendRemainsAnOperationalFailure()
    {
        QTemporaryDir directory;
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", directory.filePath(QStringLiteral("missing-backend")).toUtf8());
        DwarfsctlJob job({QString(), {QStringLiteral("build")}, 0, false});
        job.setAutoDelete(false);
        QSignalSpy result(&job, &KJob::result);
        job.start();
        QTRY_COMPARE_WITH_TIMEOUT(result.count(), 1, 5000);
        QVERIFY(job.error() >= KJob::UserDefinedError);
        QVERIFY(job.error() != KJob::KilledJobError);
        qputenv("DWARFSCTL_BIN", original);
    }
};
QTEST_MAIN(JobTest)
#include "tst_dwarfsctljob.moc"
