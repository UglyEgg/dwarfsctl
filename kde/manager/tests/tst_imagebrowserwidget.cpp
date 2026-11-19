// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "imagebrowserwidget.h"
#include <KLocalizedString>
#include <QAction>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeView>
#include <QtTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

class ImageBrowserTest final : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_directory;
    QByteArray m_original;
    QString image(const QString &name)
    {
        const auto path = m_directory.filePath(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write("image");
        return path;
    }
    QTreeView *view(ImageBrowserWidget &browser) { return browser.findChild<QTreeView *>(QStringLiteral("imageBrowserView")); }
    void activate(ImageBrowserWidget &browser, int row)
    {
        auto *tree = view(browser);
        QVERIFY(QMetaObject::invokeMethod(tree, "doubleClicked", Q_ARG(QModelIndex, tree->model()->index(row, 0))));
    }
private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("dwarfsctl");
        QVERIFY(m_directory.isValid());
        m_original = qgetenv("DWARFSCTL_BIN");
        QFile script(m_directory.filePath(QStringLiteral("backend")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/python3
import json,sys,time,os
if sys.argv[1]=='config':
    print(json.dumps(dict(ok=True,details=dict(command_timeout_ms=300000,build_timeout_ms=0,open_after_mount=True))))
    sys.exit()
image=sys.argv[2]
path=sys.argv[4]
assert sys.argv[1]=='ls' and sys.argv[3]=='--path' and sys.argv[5]=='--children-only'
if os.path.basename(image)=='slow': time.sleep(0.4)
def e(name,mode,size=None):
    return dict(name=name,path=('/' if path=='/' else path+'/')+name,mode=mode,is_dir=mode in (16877,41471),size=size)
entries=[e('folder',16877),e('large',33188,1000),e('<b>[*] Ω.dwarfs',33188,9),e('link',41471,4),e('socket',49645)] if path=='/' else []
if os.path.basename(image) in ('wide','invalid-wide'): entries=[e('entry-%05d'%i,33188,i) for i in range(2048)]
if os.path.basename(image)=='invalid-wide': entries[-1]=entries[0]
if os.path.basename(image)=='invalid': entries=[dict(name='bad',path='/a/../bad',mode=33188,size=3)]
if os.path.basename(image)=='failure' and not os.path.exists(image+'.recover'):
    print(json.dumps(dict(ok=False,command='ls',error=dict(code=3,kind='Reader',message='<b>cannot read image</b>'),details={})))
    sys.exit(3)
print(json.dumps(dict(ok=True,command='ls',details=dict(entries=entries))))
)PY");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("DWARFSCTL_BIN", script.fileName().toUtf8());
    }
    void cleanupTestCase() { qputenv("DWARFSCTL_BIN", m_original); }
    void orderingSelectionAndNavigation()
    {
        ImageBrowserWidget browser;
        QAction extract(QStringLiteral("Extract Selected…"), &browser);
        browser.setExtractAction(&extract);
        QCOMPARE(browser.findChild<QToolButton *>(QStringLiteral("extractSelectedButton"))->defaultAction(), &extract);
        const auto filename = image(QStringLiteral("normal"));
        browser.setImage(filename);
        QVERIFY(!browser.isReady());
        QVERIFY(browser.selectedPaths().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        auto *tree = view(browser);
        auto *model = tree->model();
        QCOMPARE(model->rowCount(), 5);
        QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("folder"));
        tree->sortByColumn(1, Qt::DescendingOrder);
        QCOMPARE(model->index(0, 0).data().toString(), QStringLiteral("folder"));
        QCOMPARE(model->index(1, 0).data().toString(), QStringLiteral("large"));
        QCOMPARE(model->index(2, 0).data().toString(), QStringLiteral("<b>[*] Ω.dwarfs"));
        tree->selectionModel()->select(model->index(2, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(browser.selectedPaths(), QStringList{QStringLiteral("/<b>[*] Ω.dwarfs")});
        tree->selectionModel()->select(model->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(browser.selectedPaths().size(), 2);
        QVERIFY(browser.selectedPaths().contains(QStringLiteral("/large")));
        QSignalSpy files(&browser, &ImageBrowserWidget::fileActivated);
        activate(browser, 2);
        QCOMPARE(files.count(), 1);
        QCOMPARE(files.first().first().toString(), QStringLiteral("/<b>[*] Ω.dwarfs"));
        activate(browser, 3); // A symlink remains a leaf.
        activate(browser, 4); // Special files cannot be opened as regular files.
        QCOMPARE(files.count(), 1);
        QCOMPARE(browser.currentDirectory(), QStringLiteral("/"));
        activate(browser, 0);
        QVERIFY(browser.selectedPaths().isEmpty());
        QCOMPARE(browser.currentDirectory(), QStringLiteral("/folder"));
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        QCOMPARE(model->rowCount(), 0);
        QVERIFY(browser.findChild<QLabel *>(QStringLiteral("imageBrowserState"))->text().contains(QStringLiteral("empty")));
        browser.setImage(filename); // Routine status refresh preserves navigation.
        QVERIFY(browser.isReady());
        QCOMPARE(browser.currentDirectory(), QStringLiteral("/folder"));
        browser.findChild<QToolButton *>(QStringLiteral("imageBrowserBack"))->click();
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        QCOMPARE(browser.currentDirectory(), QStringLiteral("/"));
    }
    void wideDirectoryPublishesValidatedRowsTogether()
    {
        ImageBrowserWidget browser;
        auto *proxy = qobject_cast<QSortFilterProxyModel *>(view(browser)->model());
        QVERIFY(proxy);
        QSignalSpy inserted(proxy->sourceModel(), &QAbstractItemModel::rowsInserted);
        QSignalSpy resets(proxy->sourceModel(), &QAbstractItemModel::modelReset);
        browser.setImage(image(QStringLiteral("wide")));
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        QCOMPARE(proxy->rowCount(), 2048);
        QCOMPARE(inserted.count(), 0);
        QVERIFY(resets.count() >= 1);
        QCOMPARE(proxy->index(0, 0).data().toString(), QStringLiteral("entry-00000"));
        view(browser)->sortByColumn(1, Qt::DescendingOrder);
        QCOMPARE(proxy->index(0, 0).data().toString(), QStringLiteral("entry-02047"));
        view(browser)->selectionModel()->select(proxy->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QCOMPARE(browser.selectedPaths(), QStringList{QStringLiteral("/entry-02047")});
        browser.setImage(image(QStringLiteral("invalid-wide")));
        auto *retry = browser.findChild<QToolButton *>(QStringLiteral("imageBrowserRetry"));
        QTRY_VERIFY_WITH_TIMEOUT(!retry->isHidden(), 5000);
        QVERIFY(!browser.isReady());
        QCOMPARE(proxy->rowCount(), 0);
        QVERIFY(browser.selectedPaths().isEmpty());
        QCOMPARE(inserted.count(), 0);
    }
    void failuresAreVisibleAndRetryable()
    {
        ImageBrowserWidget browser;
        const auto failedImage = image(QStringLiteral("failure"));
        browser.setImage(failedImage);
        auto *retry = browser.findChild<QToolButton *>(QStringLiteral("imageBrowserRetry"));
        QTRY_VERIFY_WITH_TIMEOUT(!retry->isHidden(), 5000);
        QVERIFY(!browser.isReady());
        QCOMPARE(view(browser)->model()->rowCount(), 0);
        auto *state = browser.findChild<QLabel *>(QStringLiteral("imageBrowserState"));
        QCOMPARE(state->textFormat(), Qt::PlainText);
        QCOMPARE(state->text(), QStringLiteral("<b>cannot read image</b>"));
        QFile recovery(failedImage + QStringLiteral(".recover"));
        QVERIFY(recovery.open(QIODevice::WriteOnly));
        recovery.close();
        retry->click();
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        browser.setImage(image(QStringLiteral("invalid")));
        QTRY_VERIFY_WITH_TIMEOUT(!retry->isHidden(), 5000);
        QVERIFY(!browser.isReady());
        QCOMPARE(view(browser)->model()->rowCount(), 0);
        browser.setImage(image(QStringLiteral("recovered")));
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
    }
    void replacementWithPreservedSizeAndMtimeInvalidatesSelection()
    {
        ImageBrowserWidget browser;
        const auto filename = image(QStringLiteral("replacement-target"));
        browser.setImage(filename);
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        auto *tree = view(browser);
        tree->selectionModel()->select(tree->model()->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(!browser.selectedPaths().isEmpty());
        struct stat before {};
        QVERIFY(::stat(QFile::encodeName(filename).constData(), &before) == 0);
        const auto replacement = image(QStringLiteral("replacement"));
        const struct timespec times[] = {before.st_atim, before.st_mtim};
        QVERIFY(::utimensat(AT_FDCWD, QFile::encodeName(replacement).constData(), times, 0) == 0);
        QVERIFY(::rename(QFile::encodeName(replacement).constData(), QFile::encodeName(filename).constData()) == 0);
        struct stat after {};
        QVERIFY(::stat(QFile::encodeName(filename).constData(), &after) == 0);
        QCOMPARE(after.st_size, before.st_size);
        QCOMPARE(after.st_mtim.tv_sec, before.st_mtim.tv_sec);
        QCOMPARE(after.st_mtim.tv_nsec, before.st_mtim.tv_nsec);
        QVERIFY(after.st_ino != before.st_ino);
        QVERIFY(!browser.isReady());
        QVERIFY(browser.selectedPaths().isEmpty());
        browser.setImage(filename);
        QVERIFY(!browser.isReady());
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        QVERIFY(browser.selectedPaths().isEmpty());
    }
    void staleCallbacksAndImageRefresh()
    {
        ImageBrowserWidget browser;
        browser.setImage(image(QStringLiteral("slow")));
        browser.setImage(QString());
        QTest::qWait(500);
        QVERIFY(!browser.isReady());
        QCOMPARE(view(browser)->model()->rowCount(), 0);
        const auto filename = image(QStringLiteral("fresh"));
        browser.setImage(filename);
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        activate(browser, 0);
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
        QFile changed(filename);
        QVERIFY(changed.open(QIODevice::Append));
        changed.write("changed");
        changed.close();
        QVERIFY(!browser.isReady());
        QVERIFY(browser.selectedPaths().isEmpty());
        browser.setImage(filename);
        QVERIFY(!browser.isReady());
        QCOMPARE(browser.currentDirectory(), QStringLiteral("/folder"));
        QTRY_VERIFY_WITH_TIMEOUT(browser.isReady(), 5000);
    }
};
QTEST_MAIN(ImageBrowserTest)
#include "tst_imagebrowserwidget.moc"
