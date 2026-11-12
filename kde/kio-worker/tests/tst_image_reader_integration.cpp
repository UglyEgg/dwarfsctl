// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// End-to-end test: real `dwarfsctl ls` output parsed by the worker's own parser.
//
// WHY THIS EXISTS SEPARATELY from tst_dwarfsctl_ls: that suite parses RECORDED
// envelopes, which proves the parser handles the documented shape but cannot
// notice if the backend's real output drifts away from it. This suite runs the
// actual backend against an actual image and feeds the result through the same
// code path the worker uses, so the two halves are checked against each other.
//
// It needs a built `dwarfsctl` and engine, so it is gated on
// DWARFSCTL_INTEGRATION=1. When the gate is off it prints
// a loud SKIP rather than passing quietly -- a silent skip reads as a pass, which
// is how an integration suite rots without anyone noticing.

#include "dwarfsctl_cat.h"
#include "dwarfsctl_ls.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace DwarfsKio;

namespace {

bool integrationEnabled()
{
    return qEnvironmentVariable("DWARFSCTL_INTEGRATION") == QLatin1String("1");
}

QString findTool(const QString &name)
{
    const QString found = QStandardPaths::findExecutable(name);
    return found;
}

/// Build a small image with mkdwarfs. Returns an empty string on failure.
QString buildFixtureImage(const QString &workDir)
{
    const QString mkdwarfs = qEnvironmentVariable("DWARFSCTL_BIN", findTool(QStringLiteral("dwarfsctl")));
    if (mkdwarfs.isEmpty()) {
        return {};
    }

    QDir dir(workDir);
    dir.mkpath(QStringLiteral("src/sub"));
    auto write = [&](const QString &rel, const QByteArray &content) {
        QFile f(dir.filePath(rel));
        if (!f.open(QIODevice::WriteOnly)) {
            return false;
        }
        f.write(content);
        return true;
    };
    write(QStringLiteral("src/hello.txt"), "hello\n");
    write(QStringLiteral("src/sub/deep.txt"), "nested\n");
    write(QStringLiteral("src/zero.txt"), "");

    const QString image = dir.filePath(QStringLiteral("fixture.dwarfs"));
    QProcess p;
    p.setProgram(mkdwarfs);
    p.setArguments({QStringLiteral("build"), dir.filePath(QStringLiteral("src")),
                    QStringLiteral("--output"), image,
                    QStringLiteral("--mkdwarfs-arg=--compress-level=4"), QStringLiteral("--mkdwarfs-arg=--no-progress")});
    p.start();
    if (!p.waitForFinished(300000) || p.exitCode() != 0) {
        return {};
    }
    return image;
}

} // namespace

class ImageReaderIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void realBackendOutputParsesIntoTheExpectedListing()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: end-to-end listing test needs DWARFSCTL_INTEGRATION=1";
            return;
        }
        if (qEnvironmentVariable("DWARFSCTL_BIN", findTool(QStringLiteral("dwarfsctl"))).isEmpty()) {
            qWarning() << "SKIP: mkdwarfs not on PATH";
            return;
        }
        if (findTool(QStringLiteral("dwarfsctl")).isEmpty()
            && qEnvironmentVariable("DWARFSCTL_BIN").isEmpty()) {
            qWarning() << "SKIP: dwarfsctl not on PATH (set DWARFSCTL_BIN to override)";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString image = buildFixtureImage(tmp.path());
        QVERIFY2(!image.isEmpty(), "could not build a fixture image with mkdwarfs");

        const ListResult r = listImage(image);
        QVERIFY2(r.ok, qPrintable(QStringLiteral("listImage failed: %1").arg(r.error)));

        // The image we just built, as the worker would see it.
        const QVector<ListedEntry> kids = childrenOf(r.entries, QStringLiteral("/"));
        QStringList names;
        for (const ListedEntry &e : kids) {
            names << e.name;
        }
        names.sort();
        // QT_NO_CAST_FROM_ASCII (set by KDECompilerSettings) makes a bare string
        // literal illegal where a QString is expected, so spell each out.
        const QStringList expected{QStringLiteral("hello.txt"),
                                   QStringLiteral("sub"),
                                   QStringLiteral("zero.txt")};
        QCOMPARE(names, expected);

        const ListedEntry *hello = entryAt(r.entries, QStringLiteral("/hello.txt"));
        QVERIFY(hello != nullptr);
        QVERIFY(hello->hasSize);
        QCOMPARE(hello->size, quint64(6));

        const ListedEntry *sub = entryAt(r.entries, QStringLiteral("/sub"));
        QVERIFY(sub != nullptr);
        QVERIFY(sub->isDir);
        // A directory carries no size -- distinct from a zero-byte file.
        QVERIFY(!sub->hasSize);

        const ListedEntry *zero = entryAt(r.entries, QStringLiteral("/zero.txt"));
        QVERIFY(zero != nullptr);
        QVERIFY(zero->hasSize);
        QCOMPARE(zero->size, quint64(0));

        QVERIFY(entryAt(r.entries, QStringLiteral("/sub/deep.txt")) != nullptr);
    }

    /// The bytes are the real bytes: materialise through the backend and read back.
    void materialiseServesRealFileContents()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: needs DWARFSCTL_INTEGRATION=1";
            return;
        }
        if (qEnvironmentVariable("DWARFSCTL_BIN", findTool(QStringLiteral("dwarfsctl"))).isEmpty()) {
            qWarning() << "SKIP: mkdwarfs not on PATH";
            return;
        }
        if (findTool(QStringLiteral("dwarfsctl")).isEmpty()
            && qEnvironmentVariable("DWARFSCTL_BIN").isEmpty()) {
            qWarning() << "SKIP: dwarfsctl not on PATH";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString image = buildFixtureImage(tmp.path());
        QVERIFY2(!image.isEmpty(), "could not build a fixture image");

        // A file with known content.
        const MaterialisedFile f = materialise(image, QStringLiteral("/hello.txt"));
        QVERIFY2(f.ok, qPrintable(QStringLiteral("materialise failed: %1").arg(f.error)));
        QVERIFY(f.size > 0);

        QByteArray out;
        QVERIFY(readWholeFile(f.localPath, f.size, &out, 64 * 1024));
        QCOMPARE(out, QByteArray("hello\n"));

        // And what streamFile yields must be identical.
        QByteArray streamed;
        QVERIFY(streamFile(f.localPath, [&](const QByteArray &c) { streamed += c; return true; }));
        QCOMPARE(streamed, out);

        // Release must remove the temp file, not leave it behind.
        const QString path = f.localPath;
        releaseMaterialised(f);
        QVERIFY2(!QFileInfo::exists(path), "releaseMaterialised left the temp file behind");
    }

    void directCopyPublishesRealFileContentsAndPreservesExistingFiles()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: needs DWARFSCTL_INTEGRATION=1";
            return;
        }
        if (qEnvironmentVariable("DWARFSCTL_BIN", findTool(QStringLiteral("dwarfsctl"))).isEmpty()) {
            qWarning() << "SKIP: dwarfsctl not on PATH";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString image = buildFixtureImage(tmp.path());
        QVERIFY2(!image.isEmpty(), "could not build a fixture image");
        const QString destination = tmp.filePath(QStringLiteral("copied.txt"));

        const CopyOutResult copied = copyOutToLocal(image,
                                                    QStringLiteral("/hello.txt"),
                                                    destination,
                                                    false);
        QVERIFY2(copied.ok, qPrintable(copied.error));
        QCOMPARE(copied.size, quint64(6));
        QFile output(destination);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("hello\n"));
        output.close();

        QVERIFY(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
        output.write("keep me\n");
        output.close();
        const CopyOutResult refused = copyOutToLocal(image,
                                                     QStringLiteral("/hello.txt"),
                                                     destination,
                                                     false);
        QVERIFY(!refused.ok);
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), QByteArray("keep me\n"));
    }

    /// A path that is not in the image must fail, never yield an empty file.
    void materialisingAMissingFileFailsRatherThanReturningNothing()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: needs DWARFSCTL_INTEGRATION=1";
            return;
        }
        if (qEnvironmentVariable("DWARFSCTL_BIN", findTool(QStringLiteral("dwarfsctl"))).isEmpty()) {
            qWarning() << "SKIP: mkdwarfs not on PATH";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString image = buildFixtureImage(tmp.path());
        QVERIFY2(!image.isEmpty(), "could not build a fixture image");

        const MaterialisedFile f = materialise(image, QStringLiteral("/definitely-not-here.txt"));
        // This is the trap the backend guards: dwarfsextract exits 0 having written
        // nothing for a pattern that matches nothing. If that leaked through, the
        // caller would receive an empty file and no error.
        QVERIFY2(!f.ok, "a missing path must not produce a successful read");
        QVERIFY2(!f.error.isEmpty(), "the failure must carry a reason");
    }

    /// A non-image must fail with the backend's reason, never as an empty listing.
    void aNonImageFailsWithTheBackendsReason()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: needs DWARFSCTL_INTEGRATION=1";
            return;
        }
        if (findTool(QStringLiteral("dwarfsctl")).isEmpty()
            && qEnvironmentVariable("DWARFSCTL_BIN").isEmpty()) {
            qWarning() << "SKIP: dwarfsctl not on PATH";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString notes = tmp.filePath(QStringLiteral("notes.txt"));
        QFile f(notes);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("this is not a dwarfs image\n");
        f.close();

        const ListResult r = listImage(notes);
        QVERIFY2(!r.ok, "a non-image must not produce a successful listing");
        QVERIFY2(!r.error.isEmpty(), "the failure must carry a reason");
        // The backend names the file; the worker passes that through rather than
        // replacing it with something generic.
        QVERIFY2(r.error.contains(QStringLiteral("notes.txt")),
                 qPrintable(QStringLiteral("error should name the file: %1").arg(r.error)));
    }
};

QTEST_MAIN(ImageReaderIntegrationTest)
#include "tst_image_reader_integration.moc"
