// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagemenuentry.h"
#include <KLocalizedString>
#include <QtTest>
using namespace dwarfsctl_tray;
class ImageMenuTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void duplicateBasenamesIncludeTheirParentFolders()
    {
        const QString first = QStringLiteral("/one/backups/archive.dwarfs");
        const QString second = QStringLiteral("/two/backups/archive.dwarfs");
        const QStringList peers{first, second};
        QCOMPARE(dwarfsctl_kde::imageParentHint(first, peers), QStringLiteral("one/backups"));
        QCOMPARE(dwarfsctl_kde::imageParentHint(second, peers), QStringLiteral("two/backups"));
        QVERIFY(imageMenuLabel(first, peers).contains(QStringLiteral("one/backups")));
        QVERIFY(imageMenuLabel(second, peers).contains(QStringLiteral("two/backups")));
        QVERIFY(imageMenuLabel(first, peers) != imageMenuLabel(second, peers));
        QCOMPARE(imageMenuLabel(first, {first}), QStringLiteral("archive"));
    }
    void parentHintsStayShortAndDistinguishNestedSuffixCollisions()
    {
        const QStringList simple{QStringLiteral("/a/sourceX/archive.dwarfs"), QStringLiteral("/a/sourceY/archive.dwarfs")};
        QCOMPARE(dwarfsctl_kde::imageParentHint(simple.at(0), simple), QStringLiteral("sourceX"));
        QCOMPARE(dwarfsctl_kde::imageParentHint(simple.at(1), simple), QStringLiteral("sourceY"));
        const QStringList nested{QStringLiteral("/a/docs/archive.dwarfs"), QStringLiteral("/b/a/docs/archive.dwarfs"),
            QStringLiteral("/c/b/a/docs/archive.dwarfs")};
        QCOMPARE(dwarfsctl_kde::imageParentHint(nested.at(0), nested), QStringLiteral("a/docs"));
        QCOMPARE(dwarfsctl_kde::imageParentHint(nested.at(1), nested), QStringLiteral("b/a/docs"));
        QCOMPARE(dwarfsctl_kde::imageParentHint(nested.at(2), nested), QStringLiteral("c/b/a/docs"));
        QVERIFY(dwarfsctl_kde::imageParentHint(simple.at(0), {simple.at(0), simple.at(0)}).isEmpty());
        QCOMPARE(dwarfsctl_kde::imageParentHint(QStringLiteral("/archive.dwarfs"),
            {QStringLiteral("/archive.dwarfs"), QStringLiteral("/nested/archive.dwarfs")}), QStringLiteral("/"));
    }
    void sameFolderCaseDistinctExtensionsRemainDistinctAcrossTheWholeGroup()
    {
        const QStringList peers{QStringLiteral("/a/Image.dwarfs"), QStringLiteral("/a/Image.DWARFS"),
            QStringLiteral("/Image.dwarfs/Image.DWARFS")};
        QSet<QString> labels;
        for (const auto &path : peers) labels.insert(imageMenuLabel(path, peers));
        QCOMPARE(labels.size(), peers.size());
        QVERIFY(imageMenuLabel(peers.first(), peers).contains(QStringLiteral("Image.dwarfs")));
        QVERIFY(imageMenuLabel(peers.at(1), peers).contains(QStringLiteral("Image.DWARFS")));
    }
    void missingSourceRoutesToUtilityWithTheExactRememberedPath()
    {
        const QString path = QStringLiteral("/unplugged/<image> & [*] Ω.dwarfs");
        const auto entry = recentImageMenuEntry(path, {path}, {}, false);
        QVERIFY(entry.action == ImageMenuAction::LocateInUtility);
        QCOMPARE(entry.target, path);
        QVERIFY(entry.label.contains(QStringLiteral("Unavailable")));
        QVERIFY(entry.label.contains(QStringLiteral("<image> & [*] Ω")));
        QVERIFY(entry.tooltip.contains(QStringLiteral("locate")));
        QVERIFY(entry.tooltip.contains(path));
    }
    void mountedFilesRemainAccessibleWhenSourceIsMissing()
    {
        const QString image = QStringLiteral("/unplugged/archive.dwarfs");
        const QString mountpoint = QStringLiteral("/run/user/1000/mounted archive");
        const auto entry = recentImageMenuEntry(image, {image}, mountpoint, false);
        QVERIFY(entry.action == ImageMenuAction::OpenMountedFolder);
        QCOMPARE(entry.target, mountpoint);
        QVERIFY(entry.tooltip.contains(QStringLiteral("source image is unavailable")));
        QVERIFY(entry.label.contains(QStringLiteral("Mounted")));
    }
    void availableUnmountedSourceCanMount()
    {
        const QString image = QStringLiteral("/images/ordinary.dwarfs");
        const auto entry = recentImageMenuEntry(image, {image}, {}, true);
        QVERIFY(entry.action == ImageMenuAction::Mount);
        QCOMPARE(entry.target, image);
        QCOMPARE(entry.label, QStringLiteral("ordinary"));
    }
};
QTEST_GUILESS_MAIN(ImageMenuTest)
#include "tst_image_menu.moc"
