// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "managerwindow.h"
#include "visualizationwidgets.h"

#include <QAction>
#include <QFrame>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QToolButton>
#include <QToolBar>
#include <QtTest>

#include <KLocalizedString>

class ManagerWindowTest final : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("dwarfsctl");
    }

    void exposesPrimarySurfaces()
    {
        DwarfsManagerWindow window;
        QVERIFY(window.findChild<QListWidget *>(QStringLiteral("imageList")));
        QVERIFY(window.findChild<QStackedWidget *>(QStringLiteral("detailPages")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("openImageAction")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("createImageAction")));
        auto *about = window.findChild<QAction *>(QStringLiteral("aboutAction"));
        auto *aboutButton = window.findChild<QToolButton *>(QStringLiteral("aboutButton"));
        QVERIFY(about && aboutButton);
        QCOMPARE(aboutButton->defaultAction(), about);
        QVERIFY(window.findChild<QToolBar *>(QStringLiteral("mainToolBar"))->isAncestorOf(aboutButton));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("forgetImageAction")));
        QVERIFY(window.findChild<QAction *>(QStringLiteral("clearRecentImagesAction")));
        QVERIFY(window.findChild<QToolButton *>(QStringLiteral("forgetImageButton")));
        QVERIFY(window.findChild<QLabel *>(QStringLiteral("sidebarSummary")));
        QVERIFY(window.findChild<QFrame *>(QStringLiteral("mountStateBox")));
        QVERIFY(window.findChild<QFrame *>(QStringLiteral("mountLocationStrip")));
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(views);
        QCOMPARE(views->count(), 2);
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("compressionOverview")));
        QVERIFY(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        auto *mount = window.findChild<QAction *>(QStringLiteral("mountAction"));
        auto *browse = window.findChild<QAction *>(QStringLiteral("browseAction"));
        auto *extract = window.findChild<QAction *>(QStringLiteral("extractAction"));
        QVERIFY(mount && browse && extract);
        QVERIFY(!mount->isEnabled());
        QVERIFY(!browse->isEnabled());
        QVERIFY(!extract->isEnabled());
    }

    void recentImageCanBeRemovedWithoutTouchingTheFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("sample.dwarfs"));
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write("not-a-real-image"), 16);
        image.close();

        DwarfsManagerWindow window;
        window.openImages({path});
        auto *images = window.findChild<QListWidget *>(QStringLiteral("imageList"));
        auto *forget = window.findChild<QAction *>(QStringLiteral("forgetImageAction"));
        auto *summary = window.findChild<QLabel *>(QStringLiteral("sidebarSummary"));
        QVERIFY(images && forget && summary);
        QCOMPARE(images->count(), 2); // Section heading plus the image.
        QVERIFY(forget->isEnabled());
        QCOMPARE(summary->text(), i18n("1 recent image"));

        forget->trigger();
        QCOMPARE(images->count(), 0);
        QCOMPARE(summary->text(), i18n("No images added"));
        QVERIFY(QFileInfo::exists(path));
    }

    void visualizationsUseExactAggregateData()
    {
        CompressionOverviewWidget compression;
        compression.setValues(105640000, 100590000);
        QCOMPARE(compression.originalBytes(), 105640000);
        QCOMPARE(compression.imageBytes(), 100590000);

        ContentRingsWidget rings;
        const QJsonObject usage{
            {QStringLiteral("schema"), QStringLiteral("dwarfsctl.usage.v1")},
            {QStringLiteral("root"), QJsonObject{
                {QStringLiteral("name"), QString()},
                {QStringLiteral("path"), QStringLiteral("/")},
                {QStringLiteral("type"), QStringLiteral("directory")},
                {QStringLiteral("size"), 42},
                {QStringLiteral("files"), 2},
                {QStringLiteral("directories"), 1},
                {QStringLiteral("children"), QJsonArray{
                    QJsonObject{{QStringLiteral("name"), QStringLiteral("Artwork")},
                                {QStringLiteral("path"), QStringLiteral("/Artwork")},
                                {QStringLiteral("type"), QStringLiteral("directory")},
                                {QStringLiteral("size"), 42},
                                {QStringLiteral("files"), 2},
                                {QStringLiteral("directories"), 1}},
                }},
            }},
        };
        QVERIFY(rings.setUsage(usage));
        QCOMPARE(rings.totalBytes(), 42);
        QCOMPARE(rings.topLevelCount(), 1);
    }

    void captureDocumentationScreenshots()
    {
        const QString directory = qEnvironmentVariable("DWARFSCTL_SCREENSHOT_DIR");
        if (directory.isEmpty())
            QSKIP("Documentation capture was not requested");
        const QString image = qEnvironmentVariable("DWARFSCTL_SCREENSHOT_IMAGE");
        QVERIFY(QFileInfo::exists(image));
        DwarfsManagerWindow window;
        window.resize(1100, 760);
        window.openImages({image});
        window.show();
        QTest::qWait(3000);
        QVERIFY(!window.findChild<QLabel *>(QStringLiteral("inlineError"))->isVisible());
        auto *rings = static_cast<ContentRingsWidget *>(window.findChild<QWidget *>(QStringLiteral("contentRings")));
        QVERIFY(rings);
        QCOMPARE(rings->topLevelCount(), 3);
        auto *views = window.findChild<QTabWidget *>(QStringLiteral("imageViews"));
        QVERIFY(views);
        views->setCurrentIndex(0);
        QTest::qWait(200);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-overview.png")));
        views->setCurrentIndex(1);
        QTest::qWait(1000);
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-contents.png")));
        window.resize(800, 620);
        QTest::qWait(200);
        QVERIFY(rings->parentWidget()->rect().contains(rings->geometry()));
        QVERIFY(window.grab().save(directory + QStringLiteral("/utility-narrow.png")));
    }
};

QTEST_MAIN(ManagerWindowTest)
#include "tst_manager_window.moc"
