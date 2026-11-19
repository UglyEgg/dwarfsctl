// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A warm unchanged listing serves exact entry lookups without another backend
// call. Cold lookups request one literal entry; image replacement and cache
// admission changes must not fabricate an empty success or truncate results.
// Local fake backends record actual invocations, so these tests need no image
// tools or installed desktop services.

#include "dwarfsctl_ls.h"

#include <QFile>
#include <QSaveFile>
#include <QDateTime>
#include <QScopeGuard>
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

QString changingBackend(QTemporaryDir &dir)
{
    const auto name = dir.filePath(QStringLiteral("changing-backend.py"));
    QFile file(name);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(R"PY(#!/usr/bin/python3
import json,os,sys
from pathlib import Path
p=Path(sys.argv[-1]);data=p.read_bytes()
if os.environ.get('DWARFSCTL_TEST_CHANGE_IMAGE')=='1' and data==b'A':
 s=p.stat();temp=p.with_name(p.name+'.replacement');temp.write_bytes(b'B');os.utime(temp,ns=(s.st_atime_ns,s.st_mtime_ns));os.replace(temp,p)
name='old.txt' if data==b'A' else 'new.txt'
print(json.dumps(dict(ok=True,details=dict(entries=[dict(name='',path='/',is_dir=True,size=None,mode=16877),dict(name=name,path='/'+name,is_dir=False,size=1,mode=33188)]))))
)PY");
    file.close();
    if (!QFile::setPermissions(name, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
    return name;
}

} // namespace

class ListingLookupTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void sameSizeAndModificationTimeReplacementInvalidatesListing()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto backend = changingBackend(root); QVERIFY(!backend.isEmpty()); BackendEnv env(backend);
        const auto image = root.filePath(QStringLiteral("image.dwarfs"));
        QFile file(image); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("A"); file.close();
        QVERIFY(entryAt(listImage(image).entries, QStringLiteral("/old.txt")));
        const QFileInfo before(image); const auto modified = before.lastModified();
        QSaveFile replacement(image); QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.write("B"); QVERIFY(replacement.commit());
        QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime)); file.close();
        QCOMPARE(QFileInfo(image).size(), before.size());
        QCOMPARE(QFileInfo(image).lastModified().toMSecsSinceEpoch(), modified.toMSecsSinceEpoch());
        const auto current = listImage(image); QVERIFY(current.ok);
        QVERIFY(entryAt(current.entries, QStringLiteral("/new.txt")));
        QVERIFY(!entryAt(current.entries, QStringLiteral("/old.txt")));
    }
    void changingImageIsNotCachedUnderItsReplacementIdentity()
    {
        QTemporaryDir root; QVERIFY(root.isValid());
        const auto backend = changingBackend(root); QVERIFY(!backend.isEmpty()); BackendEnv env(backend);
        const auto image = root.filePath(QStringLiteral("image.dwarfs"));
        QFile file(image); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("A"); file.close();
        const auto old = qgetenv("DWARFSCTL_TEST_CHANGE_IMAGE");
        const auto restore = qScopeGuard([old] { if (old.isNull()) qunsetenv("DWARFSCTL_TEST_CHANGE_IMAGE"); else qputenv("DWARFSCTL_TEST_CHANGE_IMAGE", old); });
        qputenv("DWARFSCTL_TEST_CHANGE_IMAGE", "1");
        const auto changed = listImage(image);
        QVERIFY(!changed.ok); QCOMPARE(changed.errorKind, QStringLiteral("SourceChanged"));
        qunsetenv("DWARFSCTL_TEST_CHANGE_IMAGE");
        const auto current = listImage(image); QVERIFY(current.ok);
        QVERIFY(entryAt(current.entries, QStringLiteral("/new.txt")));
        QVERIFY(!entryAt(current.entries, QStringLiteral("/old.txt")));
    }

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

    void switchingImagesEvictsThePreviousListing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString log = dir.filePath(QStringLiteral("calls.log"));
        const BackendEnv env(writeFakeBackend(dir, log));
        const QString first = dir.filePath(QStringLiteral("first.dwarfs"));
        const QString second = dir.filePath(QStringLiteral("second.dwarfs"));
        for (const auto &path : {first, second}) { QFile image(path); QVERIFY(image.open(QIODevice::WriteOnly)); image.write("image"); }
        QVERIFY(listImage(first).ok);
        QVERIFY(listImage(second).ok);
        const int before = callCount(log);
        QVERIFY(lookupEntry(first, QStringLiteral("/hello.txt")).ok);
        QCOMPARE(callCount(log), before + 1);
        QVERIFY(lastCall(log).contains(QStringLiteral("--entry-only")));
    }

    void oversizedListingsRemainCompleteAndAreNotRetained()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString backend = dir.filePath(QStringLiteral("backend"));
        QFile script(backend);
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import json,sys
image=sys.argv[-1] if '--entry-only' in sys.argv else sys.argv[2]
with open(image+'.calls','a') as log: log.write('call\n')
def entry(i):
 name='entry-%04d'%i
 return dict(name=name,path='/'+('/'.join(['segment'*32]*40))+'/'+name,is_dir=False,size=i,mode=33188)
entries=[entry(0)] if '--entry-only' in sys.argv else [entry(i) for i in range(4000)]
print(json.dumps(dict(ok=True,details=dict(entries=entries))))
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const BackendEnv env(backend);
        const QString image = dir.filePath(QStringLiteral("wide.dwarfs"));
        QFile imageFile(image);
        QVERIFY(imageFile.open(QIODevice::WriteOnly));
        imageFile.write("image"); imageFile.close();
        const auto listing = listImage(image);
        QVERIFY2(listing.ok, qPrintable(listing.error));
        QCOMPARE(listing.entries.size(), 4000);
        QCOMPARE(listing.entries.last().name, QStringLiteral("entry-3999"));
        const auto found = lookupEntry(image, listing.entries.first().path);
        QVERIFY2(found.ok, qPrintable(found.error));
        QCOMPARE(found.entries.size(), 1);
        QFile calls(image + QStringLiteral(".calls"));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().count('\n'), 2);
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
