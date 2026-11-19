// SPDX-License-Identifier: GPL-3.0-or-later
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QScopeGuard>
#include <KLocalizedString>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include "builddialog.h"
#include "bannerwidget.h"

class BuildRecommendationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void delayedFailuresKeepDraftAndRetry_data()
    {
        QTest::addColumn<QString>("stage");
        for (const auto &stage : {"ui", "creation", "schema", "profile", "recommend"})
            QTest::newRow(stage) << QString::fromLatin1(stage);
    }
    void delayedFailuresKeepDraftAndRetry()
    {
        QFETCH(QString, stage);
        QTemporaryDir directory;
        QFile backend(directory.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        QByteArray source = R"PY(#!/usr/bin/python3
import json,sys,time
from pathlib import Path
root=Path(sys.argv[0]).parent
args=sys.argv[1:]
time.sleep(.12)
if args[:2]==['config','show']: stage=args[-1]
elif args[:2]==['mkdwarfs','schema']: stage='schema'
elif args[:2]==['mkdwarfs','recommend']: stage='recommend'
else: stage='profile'
if (root/'unavailable').exists() and stage=='STAGE':
    print(json.dumps(dict(ok=False,error=dict(code=9,kind='Internal',message='Settings unavailable'))));sys.exit(9)
if stage=='ui': details=dict(command_timeout_ms=300000,build_timeout_ms=0,open_after_mount=True)
elif stage=='creation': details=dict(default_mode='MODE',default_profile='fixture',recommendation_goal='balanced')
elif stage=='schema': details=dict(schema=dict(categories=[dict(name='Basics',options=[dict(opt='--level',label='Level',kind='Int')])]))
elif stage=='profile': details=dict(profile=dict(name='fixture',args=['--level=3']))
else: details=dict(schema='dwarfsctl.recommend.v1',recommendation=dict(flags_only=['--level=3'],reasons=['Folder contents considered'],warnings=[],confidence='medium'))
print(json.dumps(dict(ok=True,details=details)))
)PY";
        source.replace("STAGE", stage.toUtf8());
        source.replace("MODE", stage == QLatin1String("recommend") ? "recommend" : "profile");
        backend.write(source);
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QFile unavailable(directory.filePath(QStringLiteral("unavailable")));
        QVERIFY(unavailable.open(QIODevice::WriteOnly));
        unavailable.close();
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        bool shellSeen = false, sawLoading = false, failedClosed = false, retried = false, ready = false;
        int beats = 0;
        QTimer driver;
        driver.setInterval(5);
        connect(&driver, &QTimer::timeout, this, [&] {
            ++beats;
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *status = dialog->findChild<QLabel *>(QStringLiteral("compressionStatus"));
            if (status && status->text().contains(QStringLiteral("Loading"))) sawLoading = true;
            auto *output = dialog->findChild<QLineEdit *>(QStringLiteral("buildOutput"));
            auto *retry = dialog->findChild<QPushButton *>(QStringLiteral("buildRetry"));
            auto *create = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
            if (!shellSeen) {
                shellSeen = true;
                output->setText(QStringLiteral("draft.dwarfs"));
            }
            if (retry->isVisible() && retry->isEnabled() && !retried) {
                failedClosed = !create->isEnabled();
                QFile::remove(unavailable.fileName());
                retried = true;
                retry->click();
            } else if (retried && create->isEnabled()) {
                ready = output->text() == QLatin1String("draft.dwarfs");
                create->click();
            }
        });
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        driver.start();
        timeout.start(10000);
        const auto result = dwarfsplugin::promptBuildOptions(nullptr, directory.path());
        driver.stop();
        timeout.stop();
        QVERIFY(shellSeen);
        QVERIFY(sawLoading);
        QVERIFY(failedClosed);
        QVERIFY(retried);
        QVERIFY(ready);
        QVERIFY(result.accepted);
        QCOMPARE(result.outputPath, QFileInfo(directory.path()).dir().filePath(QStringLiteral("draft.dwarfs")));
        QCOMPARE(result.profileArgs, QStringList{QStringLiteral("--level=3")});
        QVERIFY(beats > 10);
    }
    void configuredRecommendationAppliesAutomatically() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile backend(dir.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"(#!/bin/sh
case "$1 $2" in
 'config show')
  if [ "$4" = 'ui' ]; then printf '%s\n' '{"ok":true,"details":{"command_timeout_ms":300000,"build_timeout_ms":0,"open_after_mount":true}}'; exit 0; fi
  printf '%s\n' '{"ok":true,"details":{"default_mode":"recommend","default_profile":"default","recommendation_goal":"fast-build"},"warnings":[],"error":null}' ;;
 'mkdwarfs recommend')
  [ "$4" = '--goal' ] && [ "$5" = 'fast-build' ] || exit 3
  printf '%s\n' '{"ok":true,"command":"mkdwarfs_recommend","details":{"schema":"dwarfsctl.recommend.v1","recommendation":{"flags_only":["--example=value"],"reasons":["Favor fast creation; the finished image may be larger."],"notes":["Recommendations use file metadata and extensions; no compression benchmark was run."],"warnings":[],"confidence":"low"}},"warnings":[],"error":null}' ;;
 *) printf '%s\n' '{"ok":true,"details":{"schema":{"categories":[{"name":"Basics","options":[{"opt":"--level","label":"Level","kind":"Int"}]}]}},"warnings":[],"error":null}' ;;
esac
)" );
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const auto old = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        bool applied = false;
        bool optionsWork = false;
        bool detailsFit = false;
        bool previewHintFits = false;
        bool quiet = false;
        bool timedOut = false;
        int phase = 0;
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
                auto *more = dlg->findChild<QToolButton *>(QStringLiteral("buildMoreOptions"));
                auto *panel = dlg->findChild<QWidget *>(QStringLiteral("buildOptionsPanel"));
                auto *why = dlg->findChild<QToolButton *>(QStringLiteral("buildWhy"));
                auto *detailsPanel = dlg->findChild<QWidget *>(QStringLiteral("buildDetailsPanel"));
                auto *details = dlg->findChild<QLabel *>(QStringLiteral("buildRecommendationDetails"));
                auto *preview = dlg->findChild<QCheckBox *>(QStringLiteral("buildPreviewOnly"));
                auto *hint = dlg->findChild<QWidget *>(QStringLiteral("buildPreviewHint"));
                auto *hintDescription = dlg->findChild<QLabel *>(QStringLiteral("buildPreviewDescription"));
                auto *hintTime = dlg->findChild<QLabel *>(QStringLiteral("buildPreviewTime"));
                auto *create = dlg->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
                if (phase == 0) {
                    quiet = !dlg->findChild<dwarfsplugin::BannerWidget *>()->isVisible();
                    applied = status->text().contains(QStringLiteral("Fastest creation"))
                        && status->width() >= status->fontMetrics().horizontalAdvance(status->text())
                        && !panel->isVisible() && !detailsPanel->isVisible();
                    why->click();
                    phase = 1;
                    return;
                }
                if (phase == 1) {
                    detailsFit = detailsPanel->isVisible() && details->heightForWidth(details->width()) <= details->height();
                    more->click();
                    phase = 2;
                    return;
                }
                if (phase == 2) {
                    dlg->resize(580, dlg->height());
                    preview->setChecked(true);
                    phase = 3;
                    return;
                }
                optionsWork = panel->isVisible() && create->text() == QLatin1String("Preview");
                previewHintFits = hint->isVisible()
                    && hintDescription->heightForWidth(hintDescription->width()) <= hintDescription->height()
                    && hintTime->heightForWidth(hintTime->width()) <= hintTime->height()
                    && hint->mapTo(dlg, QPoint(0, hint->height())).y() < create->mapTo(dlg, QPoint(0, 0)).y();
                if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS"))
                    dlg->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-dialog-preview-hint.png"));
                preview->setChecked(false);
                create->click();
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
        QVERIFY(quiet);
        QVERIFY(detailsFit);
        QVERIFY(optionsWork);
        QVERIFY(previewHintFits);
        QVERIFY(result.accepted);
        QCOMPARE(result.profileName, QStringLiteral("recommended"));
        QCOMPARE(result.profileArgs, QStringList{QStringLiteral("--example=value")});
    }
};
QTEST_MAIN(BuildRecommendationTest)
#include "tst_buildrecommendation.moc"
