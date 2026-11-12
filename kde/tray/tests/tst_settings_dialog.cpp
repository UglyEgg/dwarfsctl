// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QWidget>

#include "settingsdialog.h"

class SettingsDialogTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
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
        SettingsDialog dialog;
        qputenv("DWARFSCTL_BIN", oldBackend);
        qputenv("XDG_CONFIG_HOME", oldConfigHome);

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
