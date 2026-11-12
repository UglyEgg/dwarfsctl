// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Regression tests for review finding F4: every stat() re-listed the entire
// image.
//
// The defect: `stat()` called `listImage()`, so entering an 80-file directory
// (one listDir plus up to 80 stat calls) cost up to 81 full listings -- measured
// at ~4 ms each, ~323 ms of repeated work, scaling with directory size. The fix
// has two halves: a targeted `dwarfsctl ls --path ... --entry-only` lookup for a
// cold cache, and a process-lifetime listing cache (keyed by the image file's
// size and mtime) so the stat calls that follow a listDir cost nothing at all.
//
// These tests drive a FAKE dwarfsctl -- a shell script planted at DWARFSCTL_BIN
// that appends its argv to a log file -- so the assertions count real backend
// invocations instead of trusting the implementation's account of itself. A
// cache that silently stopped working would fail here, not pass.
//
// Ungated: everything runs against a temp directory and a script, so the suite
// runs anywhere, including CI.

#include "dwarfsctl_ls.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace DwarfsKio;

namespace {

/// A fake backend that logs every invocation and answers with a fixed envelope.
///
/// The envelope distinguishes `--entry-only` (one entry) from a full listing (the
/// whole tree), mirroring the real backend, so a test can assert which form was
/// requested as well as how many times.
QString writeFakeBackend(QTemporaryDir &dir, const QString &logPath)
{
    const QString script = QStringLiteral(R"SH(#!/bin/sh
echo "$@" >> 'LOG_PATH'
case "$*" in
  *--entry-only*)
    ENTRIES='[{"name":"hello.txt","path":"/hello.txt","is_dir":false,"size":6,"mode":33188}]'
    ;;
  *)
    ENTRIES='[{"name":"","path":"/","is_dir":true,"size":null,"mode":16877},{"name":"hello.txt","path":"/hello.txt","is_dir":false,"size":6,"mode":33188}]'
    ;;
esac
printf '{"ok":true,"command":"ls","image":"/tmp/fixture.dwarfs","details":{"entries":%s},"warnings":[],"warning_objects":[],"error":null}\n' "$ENTRIES"
)SH")
        .replace(QStringLiteral("LOG_PATH"), logPath);

    const QString path = dir.filePath(QStringLiteral("dwarfsctl"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    f.write(script.toUtf8());
    f.close();
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

/// Number of backend invocations recorded in the log.
int callCount(const QString &logPath)
{
    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QByteArray content = f.readAll();
    return static_cast<int>(content.count('\n'));
}

/// Last backend invocation's argv, as one string.
QString lastCall(const QString &logPath)
{
    QFile f(logPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        if (!lines.at(i).trimmed().isEmpty()) {
            return QString::fromUtf8(lines.at(i));
        }
    }
    return {};
}

/// Scope guard: set DWARFSCTL_BIN for the duration, restore it afterwards.
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

class ListingLookupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        // The cache is process-lifetime state; start every case from empty so
        // the counts below are per-case facts, not run-order accidents.
        clearListingCache();
    }

    /// The browse pattern: one listing, then a stat per entry -- one call total.
    void aBrowseCostsOneBackendCallNotOnePerEntry()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeFakeBackend(dir, log);
        QVERIFY(!backend.isEmpty());
        const BackendEnv env(backend);

        const QString image = dir.filePath(QStringLiteral("img.dwarfs"));
        QFile img(image);
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write("image-one");
        img.close();

        // listDir(): the one listing.
        const ListResult listing = listImage(image);
        QVERIFY2(listing.ok, qPrintable(listing.error));
        QCOMPARE(callCount(log), 1);

        // The stat calls that follow: each resolves from the cache. This is the
        // regression: before the fix each of these re-listed the whole image.
        for (int i = 0; i < 5; ++i) {
            const ListResult one = lookupEntry(image, QStringLiteral("/hello.txt"));
            QVERIFY2(one.ok, qPrintable(one.error));
            QCOMPARE(one.entries.size(), 1);
            QCOMPARE(one.entries.first().path, QStringLiteral("/hello.txt"));
            QCOMPARE(one.entries.first().size, quint64(6));
        }
        QCOMPARE(callCount(log), 1);
    }

    /// A stat on a cold cache narrows to one entry instead of the whole tree.
    void aColdLookupAsksForExactlyOneEntry()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeFakeBackend(dir, log);
        QVERIFY(!backend.isEmpty());
        const BackendEnv env(backend);

        const QString image = dir.filePath(QStringLiteral("img.dwarfs"));
        QFile img(image);
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write("image-one");
        img.close();

        const ListResult one = lookupEntry(image, QStringLiteral("/hello.txt"));
        QVERIFY2(one.ok, qPrintable(one.error));
        QCOMPARE(callCount(log), 1);
        const QString argv = lastCall(log);
        QVERIFY2(argv.contains(QStringLiteral("--entry-only")),
                 qPrintable(QStringLiteral("cold lookup must narrow the listing: %1").arg(argv)));
        QVERIFY2(argv.contains(QStringLiteral("--path /hello.txt")),
                 qPrintable(QStringLiteral("cold lookup must name the path: %1").arg(argv)));
    }

    /// A missing path against a warm cache is GenuineAbsence, with no call.
    ///
    /// The cached listing is the whole tree, so a miss there is a real not-found
    /// -- and it must be an InvalidPath failure, never an empty success, and never
    /// a fresh full listing to re-learn what is already known.
    void aWarmLookupOfAMissingPathFailsWithoutACall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeFakeBackend(dir, log);
        QVERIFY(!backend.isEmpty());
        const BackendEnv env(backend);

        const QString image = dir.filePath(QStringLiteral("img.dwarfs"));
        QFile img(image);
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write("image-one");
        img.close();

        QVERIFY(listImage(image).ok);
        QCOMPARE(callCount(log), 1);

        const ListResult missing = lookupEntry(image, QStringLiteral("/nope.txt"));
        QVERIFY2(!missing.ok, "absence must not render as a successful empty result");
        QCOMPARE(missing.errorKind, QStringLiteral("InvalidPath"));
        QVERIFY2(!missing.error.isEmpty(), "the failure must carry a reason");
        QCOMPARE(callCount(log), 1);
    }

    /// A replaced image is a different image: the cache must not answer for it.
    void aChangedImageInvalidatesTheCache()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const QString backend = writeFakeBackend(dir, log);
        QVERIFY(!backend.isEmpty());
        const BackendEnv env(backend);

        const QString image = dir.filePath(QStringLiteral("img.dwarfs"));
        QFile img(image);
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write("image-one");
        img.close();

        QVERIFY(listImage(image).ok);
        QCOMPARE(callCount(log), 1);

        // Rebuild the image: different size (and mtime), so the key no longer
        // matches and the next call must go back to the backend rather than serve
        // a stale listing.
        QVERIFY(img.open(QIODevice::WriteOnly | QIODevice::Truncate));
        img.write("image-two-is-larger");
        img.close();

        const ListResult fresh = lookupEntry(image, QStringLiteral("/hello.txt"));
        QVERIFY2(fresh.ok, qPrintable(fresh.error));
        QCOMPARE(callCount(log), 2);
    }

    /// The failure path must not poison the cache with a fake empty listing.
    void aFailedListingIsNeverCachedAsEmpty()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));

        // A backend that always fails, printing the real tool's error shape.
        const QString script = QStringLiteral(R"SH(#!/bin/sh
echo "$@" >> 'LOG_PATH'
printf '{"ok":false,"command":"ls","image":"/tmp/broken.dwarfs","details":null,"warnings":[],"warning_objects":[],"error":{"code":4,"kind":"InvalidPath","message":"not a DwarFS image"}}\n'
exit 4
)SH")
                                   .replace(QStringLiteral("LOG_PATH"), log);
        const QString backend = dir.filePath(QStringLiteral("dwarfsctl"));
        QFile f(backend);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(script.toUtf8());
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        const BackendEnv env(backend);

        const QString image = dir.filePath(QStringLiteral("broken.dwarfs"));
        QFile img(image);
        QVERIFY(img.open(QIODevice::WriteOnly));
        img.write("not really an image");
        img.close();

        const ListResult first = listImage(image);
        QVERIFY2(!first.ok, "a failed listing must fail");
        QCOMPARE(callCount(log), 1);

        // Not cached: a second attempt must ask the backend again rather than
        // serving an empty success from memory.
        const ListResult second = listImage(image);
        QVERIFY2(!second.ok, "a failed listing must not be cached as an empty success");
        QCOMPARE(callCount(log), 2);
    }
};

QTEST_MAIN(ListingLookupTest)
#include "tst_listing_lookup.moc"
