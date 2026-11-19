// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the `dwarfsctl cat` client.
//
// Deliberately UNGATED: every case here works against local temp files, or points
// the client at a binary that cannot exist, so the whole suite runs everywhere --
// including CI, which has no DwarFS tools. The end-to-end coverage that DOES need
// a real image lives in tst_image_reader_integration.cpp behind
// DWARFSCTL_INTEGRATION=1.
//
// The behaviour that matters most: a failure must never look like an empty file.
// "the file is empty" and "the file could not be read" are different facts, and
// collapsing them is the fabricated-success class this project has shipped twice.

#include "dwarfsctl_cat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

using namespace DwarfsKio;

namespace {

QString writeTempFile(QTemporaryDir &dir, const QString &name, const QByteArray &content)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return {};
    }
    f.write(content);
    f.close();
    return path;
}

QString writeCopyBackend(QTemporaryDir &dir, const QString &logPath)
{
    const QString script = QStringLiteral(R"SH(#!/bin/sh
echo "$@" >> 'LOG_PATH'
if [ "$1" != cat ]; then exit 2; fi
printf 'copied directly\n' > "$4"
printf '{"ok":true,"command":"cat","details":{"bytes":16},"error":null}\n'
)SH").replace(QStringLiteral("LOG_PATH"), logPath);
    const QString path = dir.filePath(QStringLiteral("dwarfsctl-copy"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    f.write(script.toUtf8());
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

class BackendEnv
{
public:
    explicit BackendEnv(const QString &program)
        : m_previous(qgetenv("DWARFSCTL_BIN"))
    {
        qputenv("DWARFSCTL_BIN", program.toUtf8());
    }
    ~BackendEnv()
    {
        if (m_previous.isEmpty()) {
            qunsetenv("DWARFSCTL_BIN");
        } else {
            qputenv("DWARFSCTL_BIN", m_previous);
        }
    }
private:
    QByteArray m_previous;
};

} // namespace

class DwarfsctlCatTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void copyOutPublishesDirectlyAndForwardsOverwrite()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeCopyBackend(dir, log);
        QVERIFY(!backend.isEmpty());
        const BackendEnv env(backend);
        const QString destination = dir.filePath(QStringLiteral("result.txt"));

        const CopyOutResult first = copyOutToLocal(QStringLiteral("/image.dwarfs"),
                                                   QStringLiteral("/inside.txt"),
                                                   destination,
                                                   false);
        QVERIFY2(first.ok, qPrintable(first.error));
        QCOMPARE(first.size, quint64(16));
        QCOMPARE(QFile(destination).exists(), true);

        QFile logFile(log);
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        const QByteArray firstCall = logFile.readAll();
        QVERIFY(firstCall.contains("cat /image.dwarfs /inside.txt"));
        QVERIFY(!firstCall.contains("--force"));
        logFile.close();

        const CopyOutResult refused = copyOutToLocal(QStringLiteral("/image.dwarfs"),
                                                     QStringLiteral("/inside.txt"),
                                                     destination,
                                                     false);
        QVERIFY(!refused.ok);
        QCOMPARE(refused.errorKind, QStringLiteral("DestinationExists"));

        const CopyOutResult replaced = copyOutToLocal(QStringLiteral("/image.dwarfs"),
                                                      QStringLiteral("/inside.txt"),
                                                      destination,
                                                      true);
        QVERIFY2(replaced.ok, qPrintable(replaced.error));
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        QVERIFY(logFile.readAll().contains("--force"));
    }

    void copyOutRejectsADirectoryWithoutStartingTheBackend()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeCopyBackend(dir, log);
        const BackendEnv env(backend);

        const CopyOutResult result = copyOutToLocal(QStringLiteral("/image.dwarfs"),
                                                    QStringLiteral("/inside.txt"),
                                                    dir.path(),
                                                    true);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorKind, QStringLiteral("DestinationIsDirectory"));
        QVERIFY(!QFileInfo::exists(log));
    }

    void copyCancellationCleansOnlyOwnedStaging_data()
    {
        QTest::addColumn<bool>("ignoreTermination");
        QTest::newRow("cooperative") << false;
        QTest::newRow("forced-outcome-unknown") << true;
    }

    void copyCancellationCleansOnlyOwnedStaging()
    {
        QFETCH(bool, ignoreTermination);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString script = QStringLiteral(R"PY(#!/usr/bin/python3
import os,sys,time,signal
if %1: signal.signal(signal.SIGTERM,signal.SIG_IGN)
stage=os.path.join(os.environ['DWARFSCTL_COPY_STAGING_DIR'],'partial')
with open(stage,'wb') as out: out.write(b'x'*65536)
with open(sys.argv[4]+'.ready','w') as out: out.write(stage)
while True: time.sleep(0.01)
)PY").arg(ignoreTermination ? QStringLiteral("True") : QStringLiteral("False"));
        const QString backend = writeTempFile(dir, QStringLiteral("backend"), script.toUtf8());
        QVERIFY(QFile::setPermissions(backend, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const BackendEnv env(backend);
        const QString destination = dir.filePath(QStringLiteral("result"));
        QVERIFY(QDir(dir.path()).mkdir(QStringLiteral(".dwarfsctl-copy-foreign")));
        const QString foreign = writeTempFile(dir, QStringLiteral(".dwarfsctl-copy-foreign/keep"), "competitor");
        bool witnessed = false;
        const auto result = copyOutToLocal(QStringLiteral("/image"), QStringLiteral("/file"), destination, false, [&] {
            QFile ready(destination + QStringLiteral(".ready"));
            if (!ready.open(QIODevice::ReadOnly)) return false;
            const QString stage = QString::fromUtf8(ready.readAll());
            witnessed = QFileInfo(stage).size() > 0;
            return witnessed;
        });
        QVERIFY(witnessed);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorKind, ignoreTermination ? QStringLiteral("CleanupUnconfirmed") : QStringLiteral("Cancelled"));
        QVERIFY(!QFileInfo::exists(destination));
        QCOMPARE(QDir(dir.path()).entryList({QStringLiteral(".dwarfsctl-copy-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{QStringLiteral(".dwarfsctl-copy-foreign")});
        QFile kept(foreign);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("competitor"));
    }

    void publicationConflictUsesTypedReason()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString backend = writeTempFile(dir, QStringLiteral("backend"), R"SH(#!/bin/sh
printf '{"ok":false,"error":{"kind":"InvalidPath","message":"arbitrary diagnostic"},"details":{"reason":"destination_exists"}}\n'
exit 4
)SH");
        QVERIFY(QFile::setPermissions(backend, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const BackendEnv env(backend);
        const auto result = copyOutToLocal(QStringLiteral("/image"), QStringLiteral("/file"), dir.filePath(QStringLiteral("dest")), false);
        QVERIFY(!result.ok);
        QCOMPARE(result.errorKind, QStringLiteral("DestinationExists"));
        QCOMPARE(result.error, QStringLiteral("arbitrary diagnostic"));
    }

    void materialisedLeaseFollowsItsLastReference()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString backend = writeCopyBackend(dir, dir.filePath(QStringLiteral("calls")));
        const BackendEnv env(backend);
        QString path;
        std::weak_ptr<QTemporaryDir> owner;
        {
            auto first = materialise(QStringLiteral("/image"), QStringLiteral("/file"));
            QVERIFY2(first.ok, qPrintable(first.error));
            path = first.localPath;
            owner = first.directory;
            auto last = first;
            first = {};
            QVERIFY(QFileInfo::exists(path));
            QVERIFY(!owner.expired());
        }
        QVERIFY(owner.expired());
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
    }

    // --- streamFile: bounded reading, no backend needed --------------------

    void streamsAFileInChunks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("f"), QByteArray("hello\n"));
        QVERIFY(!path.isEmpty());

        QByteArray received;
        int calls = 0;
        const bool ok = streamFile(path, [&](const QByteArray &chunk) {
            ++calls;
            received += chunk;
            return true;
        });
        QVERIFY(ok);
        QCOMPARE(received, QByteArray("hello\n"));
        QVERIFY(calls >= 1);
    }

    // A file much larger than the chunk size must arrive intact and in several
    // chunks -- the size question the plan calls out explicitly.
    void streamsALargeFileInBoundedChunks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 2 MiB, comfortably more than the 256 KiB chunk.
        const QByteArray big(2 * 1024 * 1024, 'x');
        const QString path = writeTempFile(dir, QStringLiteral("big"), big);
        QVERIFY(!path.isEmpty());

        QByteArray received;
        int maxChunk = 0;
        int calls = 0;
        const bool ok = streamFile(path, [&](const QByteArray &chunk) {
            ++calls;
            maxChunk = qMax(maxChunk, chunk.size());
            received += chunk;
            return true;
        });
        QVERIFY(ok);
        QCOMPARE(received.size(), big.size());
        // No single chunk may be the whole file: that would mean it was buffered.
        QVERIFY2(maxChunk < big.size(),
                 qPrintable(QStringLiteral("a chunk was the whole file (%1 bytes)").arg(maxChunk)));
        QVERIFY2(calls > 1, "a multi-megabyte file should arrive in several chunks");
    }

    void anEmptyFileStreamsSuccessfullyWithNoChunks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("empty"), QByteArray());
        QVERIFY(!path.isEmpty());

        QByteArray received;
        const bool ok = streamFile(path, [&](const QByteArray &chunk) {
            received += chunk;
            return true;
        });
        // An empty file is a legitimate file, not a failure.
        QVERIFY(ok);
        QCOMPARE(received.size(), 0);
    }

    void aMissingFileFailsToStream()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!streamFile(dir.filePath(QStringLiteral("nope")), [](const QByteArray &) {
            return true;
        }));
        // The distinction that matters: failure is false, not an empty success.
    }

    void aCancelledStreamStopsEarlyWithoutReportingFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QByteArray big(1024 * 1024, 'y');
        const QString path = writeTempFile(dir, QStringLiteral("big"), big);
        QVERIFY(!path.isEmpty());

        int calls = 0;
        const bool ok = streamFile(path, [&](const QByteArray &) {
            ++calls;
            return calls < 2; // stop after the second chunk
        });
        // Stopping early is the caller's choice, not an error.
        QVERIFY(ok);
        QCOMPARE(calls, 2);
    }

    // --- readWholeFile: bounded, and refuses rather than truncating --------

    void readsAWholeFileWhenWithinTheLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("f"), QByteArray("hello\n"));
        QVERIFY(!path.isEmpty());

        QByteArray out;
        QVERIFY(readWholeFile(path, 6, &out, 1024));
        QCOMPARE(out, QByteArray("hello\n"));
    }

    void refusesAFileLargerThanTheLimit()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("big"), QByteArray(2048, 'z'));
        QVERIFY(!path.isEmpty());

        QByteArray out;
        // Must refuse outright. Truncating would hand back a partial file that the
        // caller cannot distinguish from a complete one.
        QVERIFY(!readWholeFile(path, 2048, &out, 1024));
        QVERIFY(out.isEmpty());
    }

    void refusesWhenTheSizeDisagreesWithTheFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("f"), QByteArray("hello\n"));
        QVERIFY(!path.isEmpty());

        QByteArray out;
        // Claim a size the file does not have; that is a detected inconsistency,
        // not something to paper over.
        QVERIFY(!readWholeFile(path, 9999, &out, 999999));
    }

    void readsAnEmptyFileAsAnEmptyByteArray()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = writeTempFile(dir, QStringLiteral("e"), QByteArray());
        QVERIFY(!path.isEmpty());

        QByteArray out;
        QVERIFY(readWholeFile(path, 0, &out, 1024));
        QCOMPARE(out.size(), 0);
    }

    void aMissingFileFailsRatherThanReturningEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QByteArray out;
        QVERIFY(!readWholeFile(dir.filePath(QStringLiteral("nope")), 0, &out, 1024));
    }

    // --- materialise: needs the real backend -------------------------------

    void materialiseFailsWithANamedReasonWhenTheBackendIsMissing()
    {
        // No gate needed: point DWARFSCTL_BIN at a path that cannot exist and the
        // failure must still be a named one rather than an empty success. This also
        // runs everywhere, which is the point.
        const QByteArray previous = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", "/nonexistent/dwarfsctl-does-not-exist");
        const MaterialisedFile f = materialise(QStringLiteral("/tmp/whatever.dwarfs"),
                                               QStringLiteral("/x.txt"));
        if (previous.isEmpty()) {
            qunsetenv("DWARFSCTL_BIN");
        } else {
            qputenv("DWARFSCTL_BIN", previous);
        }

        QVERIFY2(!f.ok, "a missing backend must not produce a successful read");
        QVERIFY2(!f.error.isEmpty(), "the failure must carry a reason");
        QVERIFY(!f.errorKind.isEmpty());
    }

    void materialiseReleasesItsTempFile()
    {
        // A failed materialise must not leave a temp directory behind. Repeat it a
        // few times and confirm nothing accumulates in the temp root.
        const QByteArray previous = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", "/nonexistent/dwarfsctl-does-not-exist");

        const QString tempRoot = QDir::tempPath();
        auto countStaging = [&]() {
            int n = 0;
            for (const QString &entry : QDir(tempRoot).entryList({QStringLiteral("dwarfs-kio-*")},
                                                                 QDir::Dirs)) {
                Q_UNUSED(entry);
                ++n;
            }
            return n;
        };
        const int before = countStaging();
        for (int i = 0; i < 3; ++i) {
            const MaterialisedFile f = materialise(QStringLiteral("/tmp/x.dwarfs"),
                                                   QStringLiteral("/x.txt"));
            QVERIFY(!f.ok);
            // Nothing to release on failure, but the call must be safe anyway.
            releaseMaterialised(f);
        }
        const int after = countStaging();

        if (previous.isEmpty()) {
            qunsetenv("DWARFSCTL_BIN");
        } else {
            qputenv("DWARFSCTL_BIN", previous);
        }

        QCOMPARE(after, before);
    }

    void releaseMaterialisedIsSafeOnADefaultConstructedValue()
    {
        // Must not crash or delete anything unexpected.
        MaterialisedFile empty;
        releaseMaterialised(empty);
        QVERIFY(true);
    }
};

QTEST_MAIN(DwarfsctlCatTest)
#include "tst_dwarfsctl_cat.moc"
