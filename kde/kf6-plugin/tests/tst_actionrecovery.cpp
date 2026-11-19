// SPDX-License-Identifier: GPL-3.0-or-later
#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KPluginFactory>
#include <QApplication>
#include <QFile>
#include <QMessageBox>
#include <QMenu>
#include <QElapsedTimer>
#include <QPluginLoader>
#include <QProgressDialog>
#include <QPushButton>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

class ActionRecoveryTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void actionsRecoverAfterTemporaryBackendFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto executable = directory.filePath(QStringLiteral("backend"));
        QFile backend(executable);
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"PY(#!/usr/bin/python3
import json, sys, time
from pathlib import Path
root=Path(sys.argv[0]).parent
if sys.argv[1:]==['tools','schema']:
    with (root/'calls').open('a') as calls: calls.write('schema\n')
    if not (root/'ready').exists(): sys.exit(9)
    print(json.dumps(dict(ok=True, details=dict(tools=[]))))
elif sys.argv[1:]==['config','show','--section','ui']:
    time.sleep(.8)
    print(json.dumps(dict(ok=True, details=dict(command_timeout_ms=300000,build_timeout_ms=0,open_after_mount=True))))
else:
    print(json.dumps(dict(ok=True, details={})))
)PY");
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        QFile image(directory.filePath(QStringLiteral("sample.dwarfs")));
        QVERIFY(image.open(QIODevice::WriteOnly));
        image.write("DWARFS");
        image.close();
        QPluginLoader loader(QStringLiteral(DWARFSCTL_ACTION_PLUGIN));
        auto *factory = qobject_cast<KPluginFactory *>(loader.instance());
        QVERIFY2(factory, qPrintable(loader.errorString()));
        QWidget parent;
        auto *plugin = factory->create<KAbstractFileItemActionPlugin>(&parent);
        QVERIFY(plugin);
        const KFileItemListProperties selection(KFileItemList{KFileItem(QUrl::fromLocalFile(image.fileName()))});
        int dialogs = 0;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, &dismiss, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *message = qobject_cast<QMessageBox *>(widget); message && message->isVisible()) {
                    ++dialogs;
                    message->accept();
                }
            }
        });
        dismiss.start(10);
        QElapsedTimer elapsed; elapsed.start();
        const auto actions = plugin->actions(selection, &parent);
        QVERIFY(elapsed.elapsed() < 100);
        QCOMPARE(actions.size(), 1);
        auto *menu = actions.first()->menu();
        QVERIFY(menu);
        auto *mount = menu->actions().first();
        auto *status = menu->actions().at(menu->actions().size() - 2);
        auto *retry = menu->actions().last();
        QVERIFY(!mount->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(retry->isVisible(), 3000);
        QVERIFY(status->isVisible());
        QCOMPARE(status->text(), QStringLiteral("DwarFS actions unavailable"));
        QCOMPARE(dialogs, 0);
        QFile ready(directory.filePath(QStringLiteral("ready")));
        QVERIFY(ready.open(QIODevice::WriteOnly)); ready.close();
        retry->trigger();
        QVERIFY(!mount->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(mount->isEnabled(), 3000);
        QVERIFY(!status->isVisible()); QVERIFY(!retry->isVisible());
        // Invoked actions show delayed, cancellable settings preparation.
        mount->trigger();
        QPointer<QProgressDialog> preparation;
        QTRY_VERIFY_WITH_TIMEOUT((preparation = parent.findChild<QProgressDialog *>()) && preparation->isVisible(), 3000);
        auto *cancel = preparation->findChild<QPushButton *>();
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(preparation.isNull(), 1000);
        QTest::qWait(900);
        QCOMPARE(dialogs, 0); // Cancellation must not dispatch a mount or show an error.
        QVERIFY(ready.remove());
        const auto reopened = plugin->actions(selection, &parent);
        auto *newMenu = reopened.first()->menu();
        QTRY_VERIFY_WITH_TIMEOUT(newMenu->actions().last()->isVisible(), 3000);
        QVERIFY(!newMenu->actions().first()->isEnabled());
        QFile calls(directory.filePath(QStringLiteral("calls")));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().count('\n'), 3);
        // Hosts may clear a still-open menu before a pending query completes.
        const auto cleared = plugin->actions(selection, &parent);
        cleared.first()->menu()->clear();
        QTest::qWait(300);
        QCOMPARE(dialogs, 0);
    }
};
QTEST_MAIN(ActionRecoveryTest)
#include "tst_actionrecovery.moc"
