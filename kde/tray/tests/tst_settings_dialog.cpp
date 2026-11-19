// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>
#include <QAction>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <QScopeGuard>
#include <QTimer>
#include <KMessageWidget>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QWidget>

#include "settingsdialog.h"

class SettingsDialogTest : public QObject {
    Q_OBJECT

    struct Backend {
        QTemporaryDir directory;
        QByteArray oldBackend = qgetenv("DWARFSCTL_BIN"), oldConfig = qgetenv("XDG_CONFIG_HOME");
        QString path(const char *name) const { return directory.filePath(QString::fromLatin1(name)); }
        void mode(const QByteArray &value) { QFile file(path("mode")); if (!file.open(QIODevice::WriteOnly)) qFatal("Cannot set test mode"); file.write(value); }
        bool prepare()
        {
            QFile backend(path("backend"));
            if (!backend.open(QIODevice::WriteOnly)) return false;
            backend.write(R"PY(#!/usr/bin/python3
import json,sys,time
from pathlib import Path
root=Path(sys.argv[0]).parent
args=sys.argv[1:]
with (root/'calls').open('a') as calls: calls.write(json.dumps(args)+'\n')
mode=(root/'mode').read_text()
state=root/'state'
config=json.loads(state.read_text()) if state.exists() else dict(ui=dict(open_after_mount=True,command_timeout_ms=300000,build_timeout_ms=0),creation=dict(default_mode='recommend',default_profile='default',recommendation_goal='balanced'))
time.sleep(.15)
if args==['config','show']:
    if mode=='config-failure':
        print(json.dumps(dict(ok=False,error=dict(code=9,kind='Internal',message='Settings unavailable'))));sys.exit(9)
    if mode=='malformed': config['ui']['open_after_mount']='yes'
    if mode=='long-timeout': config['ui']['command_timeout_ms']=4294967295
    if mode=='too-long': config['ui']['command_timeout_ms']=4294967296
    details=config
elif args[:3]==['config','mkdwarfs-profiles','list']:
    if mode=='profiles-failure':
        print(json.dumps(dict(ok=False,error=dict(code=9,kind='Internal',message='Profiles unavailable'))));sys.exit(9)
    details=dict(profiles=['@balanced','@fast','@max'])
elif args[:2]==['config','set']:
    preferences={}
    for key,value in zip(args[2::2],args[3::2]):
        parsed=(value=='true') if key=='ui.open_after_mount' else int(value) if key.endswith('_ms') else value
        preferences[key]=parsed
        section,name=key.split('.',1);config[section][name]=parsed
    state.write_text(json.dumps(config))
    if mode=='timeout': time.sleep(30)
    details=dict(preferences={} if mode=='invalid-ack' else preferences,path=str(state))
else:
    print(json.dumps(dict(ok=False,error=dict(code=2,kind='Usage',message='Unexpected request'))));sys.exit(2)
print(json.dumps(dict(ok=True,details=details)))
)PY");
            backend.close();
            if (!backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return false;
            qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
            qputenv("XDG_CONFIG_HOME", path("config").toUtf8());
            mode("normal");
            return true;
        }
        ~Backend() { if (oldBackend.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", oldBackend); if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", oldConfig); }
        int writes() const {
            QFile calls(path("calls")); if (!calls.open(QIODevice::ReadOnly)) return 0;
            int count=0;for(const auto &line:calls.readAll().split('\n')) if(const auto row=QJsonDocument::fromJson(line).array(); row.size()>1 && row.at(1).toString()==QLatin1String("set")) ++count;
            return count;
        }
    };
    static QPushButton *saveButton(SettingsDialog &dialog) { return dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok); }
private Q_SLOTS:
    void asyncPrerequisitesRecover_data()
    {
        QTest::addColumn<QByteArray>("failure");
        QTest::newRow("configuration") << QByteArray("config-failure");
        QTest::newRow("profile list") << QByteArray("profiles-failure");
        QTest::newRow("wrong types") << QByteArray("malformed");
    }
    void asyncPrerequisitesRecover()
    {
        QFETCH(QByteArray, failure);
        Backend backend;
        QVERIFY(backend.prepare());
        backend.mode(failure);
        QElapsedTimer elapsed;
        elapsed.start();
        SettingsDialog dialog;
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(!saveButton(dialog)->isEnabled());
        int heartbeats=0;
        QTimer heartbeat;
        connect(&heartbeat,&QTimer::timeout,&dialog,[&] { ++heartbeats; });
        heartbeat.start(10);
        auto *retry=dialog.findChild<QAction *>(QStringLiteral("settingsRetryAction"));
        auto *message=dialog.findChild<KMessageWidget *>(QStringLiteral("settingsStatus"));
        QVERIFY(retry && message);
        QTRY_VERIFY_WITH_TIMEOUT(retry->isEnabled(),3000);
        QVERIFY(!saveButton(dialog)->isEnabled());
        QVERIFY(message->messageType()==KMessageWidget::Error);
        QVERIFY(heartbeats>=10);
        QCOMPARE(backend.writes(),0);
        backend.mode("normal");
        retry->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(saveButton(dialog)->isEnabled(),3000);
        QCOMPARE(dialog.findChild<QCheckBox *>(QStringLiteral("openAfterMount"))->isChecked(),true);
        QCOMPARE(backend.writes(),0);
    }
    void uncertainSaveRequiresReadback_data()
    {
        QTest::addColumn<QByteArray>("failure");
        QTest::newRow("malformed acknowledgement") << QByteArray("invalid-ack");
        QTest::newRow("deadline after commit") << QByteArray("timeout");
    }
    void uncertainSaveRequiresReadback()
    {
        QFETCH(QByteArray,failure);
        Backend backend;QVERIFY(backend.prepare());
        SettingsDialog dialog;
        QSignalSpy accepted(&dialog,&QDialog::accepted);
        QTRY_VERIFY_WITH_TIMEOUT(saveButton(dialog)->isEnabled(),3000);
        auto *open=dialog.findChild<QCheckBox *>(QStringLiteral("openAfterMount"));
        open->setChecked(false);
        backend.mode(failure);
        saveButton(dialog)->click();
        auto *retry=dialog.findChild<QAction *>(QStringLiteral("settingsRetryAction"));
        QTRY_VERIFY_WITH_TIMEOUT(retry->isEnabled(),8000);
        QCOMPARE(backend.writes(),1);
        QCOMPARE(accepted.count(),0);
        QVERIFY(!saveButton(dialog)->isEnabled());
        QVERIFY(!open->isChecked());
        backend.mode("normal");
        retry->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(saveButton(dialog)->isEnabled(),3000);
        QCOMPARE(backend.writes(),1);
        QVERIFY(!open->isChecked());
        auto *message=dialog.findChild<KMessageWidget *>(QStringLiteral("settingsStatus"));
        QVERIFY(message->text().contains(QStringLiteral("match your changes")));
        QFile state(backend.path("state"));QVERIFY(state.open(QIODevice::ReadOnly));
        const auto confirmed=QJsonDocument::fromJson(state.readAll()).object();
        QVERIFY(!confirmed.value(QStringLiteral("ui")).toObject().value(QStringLiteral("open_after_mount")).toBool());
    }
    void largeTimeoutPreservedAndSuccessfulSaveConfirmed()
    {
        Backend backend;QVERIFY(backend.prepare());backend.mode("long-timeout");
        SettingsDialog dialog;
        QSignalSpy accepted(&dialog,&QDialog::accepted);
        QTRY_VERIFY_WITH_TIMEOUT(saveButton(dialog)->isEnabled(),3000);
        auto *timeout=dialog.findChild<QDoubleSpinBox *>(QStringLiteral("commandTimeout"));
        QCOMPARE(qRound64(timeout->value()*1000.0),qint64(4294967295));
        backend.mode("normal");
        saveButton(dialog)->click();
        QTRY_COMPARE_WITH_TIMEOUT(accepted.count(),1,3000);
        QCOMPARE(backend.writes(),1);
        QFile state(backend.path("state"));QVERIFY(state.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(state.readAll()).object().value(QStringLiteral("ui")).toObject().value(QStringLiteral("command_timeout_ms")).toDouble(),4294967295.0);
    }
    void presentsOutcomesBeforeImplementationChoices()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile backend(dir.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"SH(#!/bin/sh
case "$1:$2:$3" in
  'config:show:')
    printf '%s\n' '{"ok":true,"details":{"ui":{"open_after_mount":true,"command_timeout_ms":300000,"build_timeout_ms":0},"creation":{"default_mode":"recommend","default_profile":"default","recommendation_goal":"balanced"}},"warnings":[],"error":null}' ;;
  'config:mkdwarfs-profiles:list')
    printf '%s\n' '{"ok":true,"details":{"profiles":["@balanced","@fast","@max"]},"warnings":[],"error":null}' ;;
  *) exit 3 ;;
esac
)SH");
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        const QByteArray oldBackend = qgetenv("DWARFSCTL_BIN");
        const QByteArray oldConfigHome = qgetenv("XDG_CONFIG_HOME");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        qputenv("XDG_CONFIG_HOME", dir.filePath(QStringLiteral("config")).toUtf8());
        const auto restore = qScopeGuard([&] { qputenv("DWARFSCTL_BIN", oldBackend); qputenv("XDG_CONFIG_HOME", oldConfigHome); });
        SettingsDialog dialog;
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled(), 5000);

        auto *priority = dialog.findChild<QComboBox *>(QStringLiteral("creationPriority"));
        auto *description = dialog.findChild<QLabel *>(QStringLiteral("creationPriorityDescription"));
        auto *advanced = dialog.findChild<QCheckBox *>(QStringLiteral("settingsAdvancedUserToggle"));
        auto *advancedControls = dialog.findChild<QWidget *>(QStringLiteral("advancedCreationControls"));
        auto *diagnosticControls = dialog.findChild<QWidget *>(QStringLiteral("advancedDiagnosticControls"));
        auto *custom = dialog.findChild<QCheckBox *>(QStringLiteral("useCustomProfile"));
        QVERIFY(priority);
        QVERIFY(description);
        QVERIFY(advanced);
        QVERIFY(advancedControls);
        QVERIFY(diagnosticControls);
        QVERIFY(custom);

        QCOMPARE(priority->currentData().toString(), QStringLiteral("balanced"));
        QCOMPARE(priority->currentText(), QStringLiteral("Balanced (recommended)"));
        QVERIFY(!description->text().isEmpty());
        QVERIFY(!advanced->isChecked());
        QVERIFY(advancedControls->isHidden());
        QVERIFY(diagnosticControls->isHidden());

        priority->setCurrentIndex(priority->findData(QStringLiteral("smallest")));
        QVERIFY(description->text().contains(QStringLiteral("less storage")));

        advanced->setChecked(true);
        QVERIFY(!advancedControls->isHidden());
        QVERIFY(!diagnosticControls->isHidden());
        custom->setChecked(true);
        QVERIFY(!priority->isEnabled());
        QComboBox *profiles = nullptr;
        const auto profileBoxes = dialog.findChildren<QComboBox *>();
        for (auto *box : profileBoxes) {
            if (box->findData(QStringLiteral("@balanced")) >= 0)
                profiles = box;
        }
        QVERIFY(profiles);
        QCOMPARE(profiles->itemText(profiles->findData(QStringLiteral("@balanced"))),
                 QStringLiteral("Balanced (built in)"));
    }
};

QTEST_MAIN(SettingsDialogTest)
#include "tst_settings_dialog.moc"
