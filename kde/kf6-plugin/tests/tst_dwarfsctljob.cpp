// SPDX-License-Identifier: GPL-3.0-or-later
#include "dwarfsctljob.h"
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
using namespace dwarfsplugin;
class JobTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
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
