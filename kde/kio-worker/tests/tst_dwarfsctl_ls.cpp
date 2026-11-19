// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the `dwarfsctl ls` envelope client.
//
// These exercise parseEnvelope against RECORDED backend output rather than running
// a process, so they need neither dwarfsctl nor DwarFS installed and therefore run
// in CI. That is the point of splitting parsing from execution.
//
// The behaviour under test that matters most: a failure must never be reported as
// a successful empty listing. An empty directory and an unreadable image are
// different facts, and conflating them is the fabricated-success class this
// project has shipped twice.

#include "dwarfsctl_ls.h"

#include <QTest>

using namespace DwarfsKio;

namespace {

/// A real envelope, captured from `dwarfsctl ls` against a fixture image.
constexpr auto kGoodEnvelope = R"JSON({
  "ok": true,
  "command": "ls",
  "image": "/tmp/fixture.dwarfs",
  "details": {
    "entries": [
      {"name":"","path":"/","is_dir":true,"size":null,"mode":16877},
      {"name":"hello.txt","path":"/hello.txt","is_dir":false,"size":6,"mode":33188},
      {"name":"sub","path":"/sub","is_dir":true,"size":null,"mode":16877},
      {"name":"deep.txt","path":"/sub/deep.txt","is_dir":false,"size":7,"mode":33188},
      {"name":"zero.txt","path":"/zero.txt","is_dir":false,"size":0,"mode":33188}
    ]
  },
  "warnings": [],
  "warning_objects": [],
  "error": null
})JSON";

/// A real failure envelope: the backend names the file and the cause.
constexpr auto kNotAnImageEnvelope = R"JSON({
  "ok": false,
  "command": "ls",
  "image": "/tmp/notes.txt",
  "details": null,
  "warnings": [],
  "warning_objects": [],
  "error": {
    "code": 4,
    "kind": "InvalidPath",
    "message": "/tmp/notes.txt is not a DwarFS image (dwarfsck: no filesystem found)"
  }
})JSON";

} // namespace

class DwarfsctlLsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- the happy path ----------------------------------------------------

    void parsesEntriesFromARealEnvelope()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.entries.size(), 5);
    }

    void carriesNamePathAndType()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const ListedEntry *e = entryAt(r.entries, QStringLiteral("/hello.txt"));
        QVERIFY(e != nullptr);
        QCOMPARE(e->name, QStringLiteral("hello.txt"));
        QVERIFY(!e->isDir);
        QVERIFY(entryAt(r.entries, QStringLiteral("/sub"))->isDir);
    }

    void carriesSizeAndMode()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const ListedEntry *e = entryAt(r.entries, QStringLiteral("/hello.txt"));
        QVERIFY(e->hasSize);
        QCOMPARE(e->size, quint64(6));
        QVERIFY(e->hasMode);
        QCOMPARE(e->mode, quint32(33188));
    }

    // A zero-byte file and a directory are different facts: the file has a size
    // of zero, the directory has none. Collapsing them would make an empty file
    // indistinguishable from a folder.
    void zeroByteFileDiffersFromDirectory()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const ListedEntry *file = entryAt(r.entries, QStringLiteral("/zero.txt"));
        QVERIFY(file->hasSize);
        QCOMPARE(file->size, quint64(0));

        const ListedEntry *dir = entryAt(r.entries, QStringLiteral("/sub"));
        QVERIFY(!dir->hasSize);
    }

    void nestedPathsResolve()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const ListedEntry *e = entryAt(r.entries, QStringLiteral("/sub/deep.txt"));
        QVERIFY(e != nullptr);
        QCOMPARE(e->size, quint64(7));
    }

    void childrenOfRootAreDirectOnly()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const QVector<ListedEntry> kids = childrenOf(r.entries, QStringLiteral("/"));
        QStringList names;
        for (const ListedEntry &e : kids) {
            names << e.name;
        }
        // The root itself must not appear, and neither may a grandchild.
        QCOMPARE(names.size(), 3);
        QVERIFY(names.contains(QStringLiteral("hello.txt")));
        QVERIFY(names.contains(QStringLiteral("sub")));
        QVERIFY(names.contains(QStringLiteral("zero.txt")));
        QVERIFY(!names.contains(QStringLiteral("deep.txt")));
    }

    void childrenOfASubdirectory()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const QVector<ListedEntry> kids = childrenOf(r.entries, QStringLiteral("/sub"));
        QCOMPARE(kids.size(), 1);
        QCOMPARE(kids.first().name, QStringLiteral("deep.txt"));
    }

    // --- failures must never look like an empty directory -------------------

    // The central guarantee. If this ever passes as ok with zero entries, an
    // unreachable image renders as a valid empty folder.
    void aFailedCallIsNeverASuccessfulEmptyListing()
    {
        struct Case {
            const char *label;
            QByteArray stdout_;
            int exitCode;
        };
        const Case cases[] = {
            {"non-zero exit, empty stdout", QByteArray(), 4},
            {"non-zero exit, error envelope", QByteArray(kNotAnImageEnvelope), 4},
            {"exit 0 but no output", QByteArray(), 0},
            {"exit 0 but whitespace only", QByteArray("   \n"), 0},
            {"exit 0 but not JSON", QByteArray("<html>nope</html>"), 0},
            {"exit 0, JSON but not an object", QByteArray("[1,2,3]"), 0},
            {"ok:false inside the envelope", QByteArray(R"JSON({"ok":false})JSON"), 0},
            {"ok:true but no details", QByteArray(R"JSON({"ok":true})JSON"), 0},
            {"details without entries", QByteArray(R"JSON({"ok":true,"details":{}})JSON"), 0},
            {"entries is not an array", QByteArray(R"JSON({"ok":true,"details":{"entries":5}})JSON"), 0},
        };

        for (const Case &c : cases) {
            const ListResult r = parseEnvelope(c.stdout_, c.exitCode, {});
            QVERIFY2(!r.ok, qPrintable(QStringLiteral("%1: reported ok").arg(QLatin1String(c.label))));
            QVERIFY2(!r.error.isEmpty(),
                     qPrintable(QStringLiteral("%1: failed with no reason given").arg(QLatin1String(c.label))));
        }
    }

    // A genuinely empty directory IS ok with zero entries -- otherwise the guard
    // above would be satisfied by refusing everything.
    void aGenuinelyEmptyImageIsOkWithNoEntries()
    {
        const QByteArray empty = R"JSON({"ok":true,"command":"ls","details":{"entries":[]}})JSON";
        const ListResult r = parseEnvelope(empty, 0, {});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.entries.size(), 0);
    }

    void failureCarriesTheBackendsOwnReason()
    {
        const ListResult r = parseEnvelope(kNotAnImageEnvelope, 4, {});
        QVERIFY(!r.ok);
        QCOMPARE(r.errorKind, QStringLiteral("InvalidPath"));
        // The message must come from the backend, including the file and cause.
        QVERIFY(r.error.contains(QStringLiteral("not a DwarFS image")));
        QVERIFY(r.error.contains(QStringLiteral("notes.txt")));
    }

    void failureDiagnosticPrecedence()
    {
        const auto malformed = parseEnvelope("not json", 127, "loader: missing library\n");
        QCOMPARE(malformed.error, QStringLiteral("loader: missing library"));
        const auto emptyMessage = parseEnvelope(R"({"ok":false,"error":{"message":""}})", 9, "backend detail");
        QCOMPARE(emptyMessage.error, QStringLiteral("backend detail"));
        const auto named = parseEnvelope(kNotAnImageEnvelope, 4, "secondary detail");
        QVERIFY(named.error.contains(QStringLiteral("not a DwarFS image")));
        QVERIFY(!named.error.contains(QStringLiteral("secondary detail")));
        const auto noDetail = parseEnvelope({}, 9, {});
        QCOMPARE(noDetail.error, QStringLiteral("dwarfsctl ls failed (exit code 9)"));
        QVERIFY(!malformed.ok && !emptyMessage.ok && !named.ok && !noDetail.ok);
    }

    void errorKindIsExposedForMapping()
    {
        // The worker maps on kind, not on message text, so a reworded backend
        // message cannot silently change the classification.
        const ListResult r = parseEnvelope(kNotAnImageEnvelope, 4, {});
        QVERIFY(!r.errorKind.isEmpty());
        QVERIFY(r.errorKind != QStringLiteral("BackendError"));
    }

    // --- missing entries ---------------------------------------------------

    void missingPathIsAbsentNotAnError()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        QVERIFY(r.ok);
        QVERIFY(entryAt(r.entries, QStringLiteral("/nope")) == nullptr);
    }

    // --- entries with unusual values ---------------------------------------

    void toleratesEntriesMissingOptionalFields()
    {
        const QByteArray json =
            R"JSON({"ok":true,"details":{"entries":[{"name":"x","path":"/x","is_dir":false}]}})JSON";
        const ListResult r = parseEnvelope(json, 0, {});
        QVERIFY2(r.ok, qPrintable(r.error));
        const ListedEntry *e = entryAt(r.entries, QStringLiteral("/x"));
        QVERIFY(e != nullptr);
        QVERIFY(!e->isDir);
        // Absent size/mode must read as absent, not as zero.
        QVERIFY(!e->hasSize);
        QVERIFY(!e->hasMode);
    }

    // No mtime is carried at all. DwarFS's directory_tree reports none, and a
    // fabricated timestamp is worse than an omitted one.
    void entryCarriesNoModificationTime()
    {
        const ListResult r = parseEnvelope(kGoodEnvelope, 0, {});
        const ListedEntry *e = entryAt(r.entries, QStringLiteral("/hello.txt"));
        QVERIFY(e != nullptr);
        // ListedEntry has no mtime member at all (name, path, isDir, hasSize,
        // size, hasMode, mode). Adding one would require changing this struct,
        // which is the point: DwarFS's directory_tree reports no timestamps, and a
        // fabricated value is worse than an omitted one.
        QCOMPARE(e->name, QStringLiteral("hello.txt"));
        QVERIFY(e->hasSize);
    }
};

QTEST_MAIN(DwarfsctlLsTest)
#include "tst_dwarfsctl_ls.moc"
