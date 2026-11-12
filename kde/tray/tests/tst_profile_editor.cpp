// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTemporaryDir>

#include "profileeditordialog.h"

class ProfileEditorTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void presentsOneCompleteEditorAndRoundTripsProfile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile backend(dir.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"SH(#!/bin/sh
case "$1 $2 $3" in
  'config show --section')
    printf '%s\n' '{"ok":true,"details":{"command_timeout_ms":300000,"build_timeout_ms":0,"open_after_mount":true},"warnings":[],"error":null}' ;;
  'mkdwarfs schema ')
    printf '%s\n' '{"ok":true,"details":{"schema":{"schema_version":1,"supported_mkdwarfs_version":"0.15.7","categories":[{"name":"Basics","options":[{"opt":"--compress-level","label":"Compression level","kind":"Int","default":7,"min_int":0,"max_int":9,"repeatable":false,"optional_value":false,"simple_control":"compression-priority"}]},{"name":"Filters","options":[{"opt":"\u002d\u002dfilter","label":"Filter rule","kind":"MultiString","default":"","repeatable":true,"optional_value":false}]},{"name":"Incompressible","options":[{"opt":"--incompressible-zstd-level","label":"ZSTD compression level","kind":"Int","default":-1,"min_int":-131072,"max_int":22,"repeatable":false,"optional_value":false}]}]}},"warnings":[],"error":null}' ;;
  'config mkdwarfs-profiles get')
    printf '%s\n' '{"ok":true,"details":{"profile":{"args":["--incompressible-zstd-level","-2","\u002d\u002dfilter","+ *.txt","\u002d\u002dfilter","- *.tmp"]}},"warnings":[],"error":null}' ;;
  *) exit 3 ;;
esac
)SH");
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

        const QByteArray oldBackend = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        qputenv("DWARFSCTL_BIN", oldBackend);

        auto *simpleLevel = dialog.findChild<QComboBox *>(QStringLiteral("simpleCompressionLevel"));
        auto *summary = dialog.findChild<QLabel *>(QStringLiteral("exactSettingsSummary"));
        auto *exactSettings = dialog.findChild<QWidget *>(QStringLiteral("exactProfileSettings"));
        QVERIFY(!dialog.findChild<QCheckBox *>(QStringLiteral("advancedUserToggle")));
        QVERIFY(!dialog.findChild<QWidget *>(QStringLiteral("profileModeStack")));
        QVERIFY(simpleLevel);
        QVERIFY(summary);
        QVERIFY(exactSettings);
        QCOMPARE(simpleLevel->currentIndex(), 0);
        QCOMPARE(simpleLevel->currentText(), QStringLiteral("Balanced (recommended)"));
        QCOMPARE(simpleLevel->count(), 4);
        QCOMPARE(simpleLevel->findData(7), -1);
        QVERIFY(summary->text().contains(QStringLiteral("2 exact settings")));

        auto *negativeLevel = dialog.findChild<QSpinBox *>(QStringLiteral("editor_incompressible_zstd_level"));
        auto *filters = dialog.findChild<QPlainTextEdit *>(QStringLiteral("editor_filter"));
        auto *preview = dialog.findChild<QPlainTextEdit *>(QStringLiteral("profileArgumentsPreview"));
        QVERIFY(negativeLevel);
        QVERIFY(filters);
        QVERIFY(preview);
        QCOMPARE(negativeLevel->value(), -2);
        QCOMPARE(filters->toPlainText(), QStringLiteral("+ *.txt\n- *.tmp"));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--incompressible-zstd-level=-2")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--" "filter=+ *.txt")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--" "filter=- *.tmp")));

        const int levelNine = simpleLevel->findData(9);
        QVERIFY(levelNine > 0);
        simpleLevel->setCurrentIndex(levelNine);
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--compress-level=9")));
        // Selecting a simple value never discards settings that are currently hidden.
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--incompressible-zstd-level=-2")));

        const int levelZero = simpleLevel->findData(0);
        QVERIFY(levelZero > 0);
        QCOMPARE(simpleLevel->itemText(levelZero), QStringLiteral("Faster creation"));

        auto *exactLevel = dialog.findChild<QSpinBox *>(QStringLiteral("editor_compress_level"));
        auto *exactLevelEnabled = dialog.findChild<QCheckBox *>(QStringLiteral("option_compress_level"));
        QVERIFY(exactLevel);
        QVERIFY(exactLevelEnabled);
        exactLevelEnabled->setChecked(true);
        exactLevel->setValue(4);
        QCOMPARE(simpleLevel->currentData().toString(), QStringLiteral("__custom__"));
    }
};

QTEST_MAIN(ProfileEditorTest)
#include "tst_profile_editor.moc"
