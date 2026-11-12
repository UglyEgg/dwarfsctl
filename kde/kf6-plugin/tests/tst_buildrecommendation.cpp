// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include "builddialog.h"

class BuildRecommendationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void configuredRecommendationAppliesAutomatically() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile backend(dir.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"(#!/bin/sh
case "$1 $2" in
 'config show')
  printf '%s\n' '{"ok":true,"details":{"default_mode":"recommend","default_profile":"default","recommendation_goal":"fast-build"},"warnings":[],"error":null}' ;;
 'mkdwarfs recommend')
  [ "$4" = '--goal' ] && [ "$5" = 'fast-build' ] || exit 3
  printf '%s\n' '{"ok":true,"command":"mkdwarfs_recommend","details":{"schema":"dwarfsctl.recommend.v1","recommendation":{"flags_only":["--example=value"],"reasons":["Fixture reason"],"warnings":["Fixture warning"],"confidence":"low"}},"warnings":[],"error":null}' ;;
 *) printf '%s\n' '{"ok":true,"details":{},"warnings":[],"error":null}' ;;
esac
)" );
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto old = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        bool applied = false;
        bool timedOut = false;
        QTimer driver;
        driver.setInterval(20);
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [&]() {
            timedOut = true;
            if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dlg->reject();
        });
        connect(&driver, &QTimer::timeout, this, [&]() {
            auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dlg) return;
            auto *status = dlg->findChild<QLabel *>(QStringLiteral("compressionStatus"));
            if (status && status->text().contains(QStringLiteral("Optimized for"))) {
                applied = status->text().contains(QStringLiteral("Fastest creation"))
                    && !status->text().contains(QStringLiteral("confidence"));
                dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            }
        });
        driver.start();
        timeout.start(10000);
        const auto result = dwarfsplugin::promptBuildOptions(nullptr, dir.path());
        driver.stop();
        timeout.stop();
        qputenv("DWARFSCTL_BIN", old);
        QVERIFY(!timedOut);
        QVERIFY(applied);
        QVERIFY(result.accepted);
        QCOMPARE(result.profileName, QStringLiteral("recommended"));
        QCOMPARE(result.profileArgs, QStringList{QStringLiteral("--example=value")});
    }
};
QTEST_MAIN(BuildRecommendationTest)
#include "tst_buildrecommendation.moc"
