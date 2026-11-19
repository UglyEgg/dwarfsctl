// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagehistory.h"
#include "../src/traysettings.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QScopeGuard>
#include <fcntl.h>
#include <sys/stat.h>
#include <QTest>
#include <QThread>
#include <memory>
#include <cstdio>
#include <vector>

using namespace dwarfsctl_kde;
class ImageHistoryTest final : public QObject {
    Q_OBJECT
    QTemporaryDir home;
    QByteArray original;
    int serial = 0;
    QString image(const QString &name) const { return home.filePath(name); }
    QString legacyPath() const { return QFileInfo(imageHistoryPath()).dir().filePath(QStringLiteral("manager.ini")); }
    void seed(const QStringList &tray, const QStringList &manager, int limit = 5)
    {
        QSettings settings(imageHistoryPath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("recent/images"), tray);
        settings.setValue(QStringLiteral("recent/limit"), limit);
        settings.setValue(QStringLiteral("ui/advanced-user"), true);
        settings.sync();
        QSettings legacy(legacyPath(), QSettings::IniFormat);
        legacy.setValue(QStringLiteral("recent/images"), manager);
        legacy.setValue(QStringLiteral("view/selected"), QStringLiteral("files"));
        legacy.sync();
    }
private Q_SLOTS:
    void initTestCase() { QVERIFY(home.isValid()); original = qgetenv("XDG_CONFIG_HOME"); }
    void init() { qputenv("XDG_CONFIG_HOME", home.filePath(QString::number(++serial)).toUtf8()); }
    void cleanupTestCase() { if (original.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", original); }
    void legacyAliasesCanonicalizeOnceAndStoredReadsStayLexical()
    {
        QFile actual(image("migration-actual.dwarfs")); QVERIFY(actual.open(QIODevice::WriteOnly)); actual.close();
        const auto alias = image("migration-alias.dwarfs"); QVERIFY(QFile::link(actual.fileName(), alias));
        const auto previous = QDir::currentPath();
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        QVERIFY(QDir::setCurrent(home.path()));
        seed({QStringLiteral("migration-alias.dwarfs"), QStringLiteral("./migration-actual.dwarfs")}, {});
        QCOMPARE(recentImages(), QStringList{actual.fileName()});
        QSettings settings(imageHistoryPath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("recent/images"), QStringList{alias}); settings.sync();
        // Existing stored identities are never re-resolved during reads or
        // unrelated preference edits; incoming remember/replace still resolve.
        QCOMPARE(recentImages(), QStringList{alias});
        QVERIFY(setRecentImageLimit(4));
        QCOMPARE(recentImages(), QStringList{alias});
    }
    void migrationPreservesTrayOrderThenManagerAndUnrelatedPreferences()
    {
        seed({image("tray"), image("shared")}, {image("manager"), image("shared"), image("missing")});
        QCOMPARE(recentImages(), QStringList({image("tray"), image("shared"), image("manager"), image("missing")}));
        QSettings tray(imageHistoryPath(), QSettings::IniFormat);
        QSettings manager(legacyPath(), QSettings::IniFormat);
        QVERIFY(tray.value(QStringLiteral("ui/advanced-user")).toBool());
        QVERIFY(tray.value(QStringLiteral("recent/manager-migrated-v1")).toBool());
        QVERIFY(!manager.contains(QStringLiteral("recent/images")));
        QCOMPARE(manager.value(QStringLiteral("view/selected")).toString(), QStringLiteral("files"));
    }
    void offDoesNotImportOrRecordAndClearNeverResurrectsLegacy()
    {
        seed({image("tray")}, {image("private")}, 0);
        QCOMPARE(recentImageLimit(), 0);
        QVERIFY(recentImages().isEmpty());
        QVERIFY(rememberImage(image("not-recorded")));
        QVERIFY(recentImages().isEmpty());
        QVERIFY(setRecentImageLimit(5));
        QVERIFY(recentImages().isEmpty());
        QVERIFY(rememberImage(image("visible")));
        QVERIFY(clearRecentImages());
        // Even an old process rewriting manager history cannot revive it once
        // the migration marker has committed. Only that obsolete key is removed.
        {
            QSettings legacy(legacyPath(), QSettings::IniFormat);
            legacy.setValue(QStringLiteral("recent/images"), QStringList{image("resurrected")});
            legacy.sync();
        }
        QVERIFY(recentImages().isEmpty());
        QSettings legacy(legacyPath(), QSettings::IniFormat);
        QVERIFY(!legacy.contains(QStringLiteral("recent/images")));
    }
    void clearBeforeFirstReadDoesNotImportLegacy()
    {
        seed({image("tray")}, {image("legacy")});
        QVERIFY(clearRecentImages());
        QVERIFY(recentImages().isEmpty());
        QVERIFY(setRecentImageLimit(9));
        QVERIFY(recentImages().isEmpty());
    }
    void limitsClampAndTrimAndChangingToOffClearsBothStores()
    {
        seed({image("a"), image("b"), image("c")}, {image("d")});
        QVERIFY(setRecentImageLimit(2));
        QCOMPARE(recentImages(), QStringList({image("a"), image("b")}));
        QVERIFY(setRecentImageLimit(99)); QCOMPARE(recentImageLimit(), 9);
        QVERIFY(setRecentImageLimit(-2)); QCOMPARE(recentImageLimit(), 0);
        QVERIFY(recentImages().isEmpty());
        QSettings manager(legacyPath(), QSettings::IniFormat);
        QVERIFY(!manager.contains(QStringLiteral("recent/images")));
    }
    void existingAliasesDeduplicateButMissingAndCaseDistinctPathsRemain()
    {
        QFile actual(image("Case.dwarfs")); QVERIFY(actual.open(QIODevice::WriteOnly)); actual.close();
        QFile lower(image("case.dwarfs")); QVERIFY(lower.open(QIODevice::WriteOnly)); lower.close();
        const QString alias = image("alias.dwarfs"); QVERIFY(QFile::link(actual.fileName(), alias));
        QVERIFY(setRecentImageLimit(9));
        QVERIFY(rememberImage(actual.fileName()));
        QVERIFY(rememberImage(alias));
        QCOMPARE(recentImages(), QStringList{actual.fileName()});
        QVERIFY(rememberImage(lower.fileName()));
        QVERIFY(rememberImage(image("gone.dwarfs")));
        QCOMPARE(recentImages(), QStringList({image("gone.dwarfs"), lower.fileName(), actual.fileName()}));
        QVERIFY(forgetImage(alias));
        QCOMPARE(recentImages(), QStringList({image("gone.dwarfs"), lower.fileName()}));
        QVERIFY(!rememberImage(QString()));
    }
    void replacementPreservesPositionAndDeduplicates()
    {
        seed({image("a"), image("old"), image("tail")}, {});
        QVERIFY(replaceRecentImage(image("old"), image("new")));
        QCOMPARE(recentImages(), QStringList({image("a"), image("new"), image("tail")}));
        QVERIFY(replaceRecentImage(image("new"), image("a")));
        QCOMPARE(recentImages(), QStringList({image("a"), image("tail")}));
        QVERIFY(replaceRecentImage(image("unknown"), image("not-added")));
        QCOMPARE(recentImages(), QStringList({image("a"), image("tail")}));
    }
    void cachedMalformedLegacyPreferencesNeverMigrateOrRewrite()
    {
        const QString kept = image("kept");
        seed({kept}, {image("must-not-import")});
        QFile legacy(legacyPath());
        QVERIFY(legacy.open(QIODevice::ReadOnly));
        const QByteArray corrupt = legacy.readAll() + QByteArray("\n[broken\n"); legacy.close();
        QVERIFY(legacy.open(QIODevice::WriteOnly)); QCOMPARE(legacy.write(corrupt), corrupt.size()); legacy.close();
        QSettings preferences(legacyPath(), QSettings::IniFormat);
        preferences.allKeys(); // Prime the process-local QSettings cache first.
        QVERIFY(recentImages().isEmpty());
        QVERIFY(!rememberImage(image("new")));
        QVERIFY(!clearRecentImages());
        QVERIFY(!dwarfsctl_tray::clearRecentlyMountedImages());
        QSettings tray(imageHistoryPath(), QSettings::IniFormat);
        QVERIFY(!tray.value(QStringLiteral("recent/manager-migrated-v1"), false).toBool());
        QCOMPARE(tray.value(QStringLiteral("recent/images")).toStringList(), QStringList{kept});
        QVERIFY(legacy.open(QIODevice::ReadOnly)); QCOMPARE(legacy.readAll(), corrupt);
    }
    void migratedHistoryIgnoresMalformedUnrelatedManagerPreferences()
    {
        const QString kept = image("kept");
        QVERIFY(rememberImage(kept));
        const QByteArray corrupt("[broken\nrecent/images=private\n");
        QFile legacy(legacyPath());
        QVERIFY(legacy.open(QIODevice::WriteOnly));
        QCOMPARE(legacy.write(corrupt), corrupt.size()); legacy.close();
        // A fresh process avoids QSettings' process-local parse/error cache
        // masking a first-read failure after another API already read the file.
        QProcess reader;
        reader.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--read-images")});
        QVERIFY(reader.waitForFinished(5000)); QCOMPARE(reader.exitCode(), 0);
        QCOMPARE(QJsonDocument::fromJson(reader.readAllStandardOutput()).array(), QJsonArray{kept});
        QProcess writer;
        writer.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--remember"), image("new")});
        QVERIFY(writer.waitForFinished(5000)); QCOMPARE(writer.exitCode(), 0);
        QCOMPARE(recentImages(), QStringList({image("new"), kept}));
        QVERIFY(setRecentImageLimit(0));
        QVERIFY(recentImages().isEmpty());
        QVERIFY(clearRecentImages());
        QVERIFY(legacy.open(QIODevice::ReadOnly)); QCOMPARE(legacy.readAll(), corrupt);
    }
    void authoritativeReplacementWithPreservedSizeAndTimeRefreshesOffAndImages()
    {
        const QString first = image("one"), second = image("two");
        seed({first}, {}, 5);
        QSettings cached(imageHistoryPath(), QSettings::IniFormat);
        cached.allKeys();
        const auto replace = [&](const QByteArray &before, const QByteArray &after) {
            if (before.size() != after.size() || before == after) return false;
            struct stat identity {};
            if (::stat(QFile::encodeName(imageHistoryPath()).constData(), &identity) != 0) return false;
            QSaveFile output(imageHistoryPath());
            if (!output.open(QIODevice::WriteOnly) || output.write(after) != after.size() || !output.commit()) return false;
            const struct timespec times[] = {identity.st_atim, identity.st_mtim};
            return ::utimensat(AT_FDCWD, QFile::encodeName(imageHistoryPath()).constData(), times, 0) == 0;
        };
        QFile file(imageHistoryPath()); QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll(); file.close();
        QVERIFY(replace(original, QByteArray(original).replace(first.toUtf8(), second.toUtf8())));
        QCOMPARE(recentImages(), QStringList{second});
        QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray current = file.readAll(); file.close();
        QVERIFY(replace(current, QByteArray(current).replace("limit=5", "limit=0")));
        QVERIFY(recentImages().isEmpty());
        QCOMPARE(recentImageLimit(), 0);
    }
    void retargetedUserConfigSymlinkAbortsBeforeChangingEitherTarget()
    {
        seed({image("old")}, {}, 9);
        const QString configured = imageHistoryPath();
        const QString original = image("retarget-original.ini"), alternate = image("retarget-alternate.ini");
        QVERIFY(QFile::rename(configured, original)); QVERIFY(QFile::link(original, configured));
        QFile old(original); QVERIFY(old.open(QIODevice::ReadOnly)); const QByteArray before = old.readAll(); old.close();
        QFile other(alternate); QVERIFY(other.open(QIODevice::WriteOnly)); other.write("[unrelated]\nkeep=untouched\n"); other.close();
        QVERIFY(other.open(QIODevice::ReadOnly)); const QByteArray protectedBytes = other.readAll(); other.close();
        bool retargeted = false;
        const bool result = image_history_detail::edit([&](image_history_detail::SettingsValues &settings) {
            settings.setValue(QStringLiteral("recent/images"), QStringList{image("new")});
            retargeted = QFile::remove(configured) && QFile::link(alternate, configured);
        });
        QVERIFY(retargeted); QVERIFY(!result);
        QVERIFY(old.open(QIODevice::ReadOnly)); QCOMPARE(old.readAll(), before);
        QVERIFY(other.open(QIODevice::ReadOnly)); QCOMPARE(other.readAll(), protectedBytes);
        QVERIFY(QFileInfo(configured).isSymLink());
    }
    void concurrentProcessesDoNotLoseDistinctUpdates()
    {
        QVERIFY(setRecentImageLimit(9));
        const QString gate = home.filePath(QStringLiteral("go"));
        std::vector<std::unique_ptr<QProcess>> writers;
        for (int i = 0; i < 9; ++i) {
            auto writer = std::make_unique<QProcess>();
            writer->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--writer"), image(QStringLiteral("writer-%1").arg(i)), gate});
            QVERIFY(writer->waitForStarted(3000));
            writers.push_back(std::move(writer));
        }
        QFile release(gate); QVERIFY(release.open(QIODevice::WriteOnly)); release.close();
        QStringList failures;
        for (auto &writer : writers) {
            if (!writer->waitForFinished(10000)) { writer->kill(); writer->waitForFinished(1000); failures.append(QStringLiteral("writer timeout")); }
            else if (writer->exitCode() != 0) failures.append(QString::number(writer->exitCode())+QStringLiteral(": ")+QString::fromUtf8(writer->readAllStandardError()));
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        const auto images = recentImages();
        QCOMPARE(images.size(), 9);
        for (int i = 0; i < 9; ++i) QVERIFY(images.contains(image(QStringLiteral("writer-%1").arg(i))));
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == QLatin1String("--read-images")) {
        const auto bytes = QJsonDocument(QJsonArray::fromStringList(recentImages())).toJson(QJsonDocument::Compact);
        QFile output; if (!output.open(stdout, QIODevice::WriteOnly)) return 4; return output.write(bytes) == bytes.size() ? 0 : 5;
    }
    if (app.arguments().value(1) == QLatin1String("--remember")) return rememberImage(app.arguments().value(2)) ? 0 : 3;
    if (app.arguments().value(1) == QLatin1String("--writer")) {
        QElapsedTimer timeout; timeout.start();
        while (!QFileInfo::exists(app.arguments().value(3))) {
            if (timeout.elapsed() > 5000) return 2;
            QThread::msleep(5);
        }
        return rememberImage(app.arguments().value(2)) ? 0 : 3;
    }
    ImageHistoryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_imagehistory.moc"
