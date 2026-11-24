// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QElapsedTimer>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScopeGuard>
#include <QMessageBox>
#include <KLocalizedString>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTemporaryDir>

#include "profileeditordialog.h"

class ProfileEditorTest : public QObject {
    Q_OBJECT

    static QString backend(QTemporaryDir &directory)
    {
        const auto path = directory.filePath(QStringLiteral("backend"));
        QFile script(path);
        if (!script.open(QIODevice::WriteOnly)) return {};
        script.write(R"PY(#!/usr/bin/python3
import json,sys,time
from pathlib import Path
root=Path(sys.argv[0]).parent
args=sys.argv[1:]
time.sleep(.12)
if args[:2]==['config','show']: stage='ui'
elif args[:2]==['mkdwarfs','schema']: stage='schema'
else: stage=args[2]
with (root/'calls').open('a') as output: output.write(stage+'\n')
failure=root/'unavailable'
if failure.exists() and failure.read_text()==stage:
    print(json.dumps(dict(ok=False,error=dict(code=9,kind='Internal',message='Profile unavailable'))));sys.exit(9)
schema=dict(categories=[dict(name='Basics',options=[dict(opt='--compress-level',label='Compression level',kind='Int',default=7,min_int=0,max_int=9,simple_control='compression-priority'),dict(opt='--future-setting',label='Future',kind='FutureKind')])])
initial=['--compress-level=3','--future-setting=opaque']
if stage=='ui': details=dict(command_timeout_ms=300000,build_timeout_ms=0,open_after_mount=True)
elif stage=='schema': details=dict(schema=schema)
elif stage=='get':
    if (root/'deleted').exists():
        print(json.dumps(dict(ok=False,error=dict(code=4,kind='Error',message='unknown profile'))));sys.exit(4)
    details=dict(profile=json.loads((root/'saved').read_text()) if (root/'saved').exists() else dict(name=args[3],args=initial))
elif stage=='list': details=dict(profiles=[] if (root/'deleted').exists() else [json.loads((root/'saved').read_text())['name'] if (root/'saved').exists() else 'fixture'])
elif stage=='validate': details=dict(valid=True)
elif stage=='set':
    profile=dict(name=args[3],args=args[5::2]);(root/'saved').write_text(json.dumps(profile))
    mode=(root/'save-mode').read_text() if (root/'save-mode').exists() else ''
    if mode=='timeout': time.sleep(8)
    details={} if mode=='invalid' else dict(profile=profile)
elif stage=='delete':
    (root/'deleted').touch();details=dict(deleted=args[3])
    if (root/'delete-mode').exists(): time.sleep(8)
else: sys.exit(3)
print(json.dumps(dict(ok=True,details=details)))
)PY");
        script.close();
        if (!script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return {};
        return path;
    }
    static void textFile(const QString &path, const QByteArray &text)
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) file.write(text);
    }

private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void delayedFailureKeepsNameAndPreventsOverwrite_data()
    {
        QTest::addColumn<QByteArray>("stage");
        for (const auto *stage : {"ui", "schema", "get"}) QTest::newRow(stage) << QByteArray(stage);
    }
    void delayedFailureKeepsNameAndPreventsOverwrite()
    {
        QFETCH(QByteArray, stage);
        QTemporaryDir directory;
        const auto executable = backend(directory);
        QVERIFY(!executable.isEmpty());
        textFile(directory.filePath(QStringLiteral("unavailable")), stage);
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        QElapsedTimer construction;
        construction.start();
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        QVERIFY(construction.elapsed() < 150);
        dialog.show();
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QVERIFY(!save->isEnabled());
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("profileName"));
        name->setText(QStringLiteral("draft-name"));
        auto *retry = dialog.findChild<QPushButton *>(QStringLiteral("profileRetry"));
        QTRY_VERIFY_WITH_TIMEOUT(retry->isVisible() && retry->isEnabled(), 5000);
        QVERIFY(!save->isEnabled());
        QVERIFY(!dialog.result().saved);
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("saved"))));
        QFile::remove(directory.filePath(QStringLiteral("unavailable")));
        retry->click();
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        QCOMPARE(name->text(), QStringLiteral("draft-name"));
        auto *preview = dialog.findChild<QPlainTextEdit *>(QStringLiteral("profileArgumentsPreview"));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--future-setting=opaque")));
        QVERIFY(preview->toPlainText().contains(QStringLiteral("--compress-level=3")));
    }
    void saveCapturesEditsAndConfirmsBackend()
    {
        QTemporaryDir directory;
        const auto executable = backend(directory);
        QVERIFY(!executable.isEmpty());
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        dialog.show();
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("profileName"));
        name->setText(QStringLiteral("saved-draft"));
        dialog.findChild<QSpinBox *>(QStringLiteral("editor_compress_level"))->setValue(9);
        save->click();
        QVERIFY(!save->isEnabled());
        QVERIFY(!name->isEnabled());
        QVERIFY(!dialog.result().saved);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.result().saved, 5000);
        QCOMPARE(dialog.result().profileName, QStringLiteral("saved-draft"));
        QVERIFY(dialog.result().profileArgs.contains(QStringLiteral("--future-setting=opaque")));
        QFile file(directory.filePath(QStringLiteral("saved")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(saved.value(QStringLiteral("name")).toString(), QStringLiteral("saved-draft"));
        QVERIFY(saved.value(QStringLiteral("args")).toArray().contains(QJsonValue(QStringLiteral("--compress-level=9"))));
    }
    void unconfirmedSaveNeedsReadback_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("invalid acknowledgement") << QByteArray("invalid");
        QTest::newRow("timeout after write") << QByteArray("timeout");
    }
    void unconfirmedSaveNeedsReadback()
    {
        QFETCH(QByteArray, mode);
        QTemporaryDir directory;
        const auto executable = backend(directory);
        QVERIFY(!executable.isEmpty());
        textFile(directory.filePath(QStringLiteral("save-mode")), mode);
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        dialog.show();
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("profileName"));
        name->setText(QStringLiteral("saved-draft"));
        auto *level = dialog.findChild<QSpinBox *>(QStringLiteral("editor_compress_level"));
        level->setValue(9);
        int beats = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        qint64 previous = 0, largestGap = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { const auto now = elapsed.elapsed(); largestGap = qMax(largestGap, now - previous); previous = now; ++beats; });
        heartbeat.start();
        save->click();
        auto *retry = dialog.findChild<QPushButton *>(QStringLiteral("profileRetry"));
        QTRY_VERIFY_WITH_TIMEOUT(retry->isVisible() && retry->isEnabled(), 8000);
        QVERIFY(!save->isEnabled());
        QVERIFY(!dialog.result().saved);
        QCOMPARE(name->text(), QStringLiteral("saved-draft"));
        QCOMPARE(level->value(), 9);
        QCOMPARE(retry->text(), QStringLiteral("Reload"));
        retry->click();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.result().saved, 5000);
        QVERIFY(save->isEnabled());
        QCOMPARE(name->text(), QStringLiteral("saved-draft"));
        QCOMPARE(level->value(), 9);
        QFile calls(directory.filePath(QStringLiteral("calls")));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().split('\n').count(QByteArray("set")), 1);
        QVERIFY(beats > 20);
        QVERIFY2(largestGap < 150, qPrintable(QStringLiteral("GUI paused %1 ms").arg(largestGap)));
    }

    void validationFailureNeverWrites()
    {
        QTemporaryDir directory;
        const auto executable = backend(directory);
        QVERIFY(!executable.isEmpty());
        textFile(directory.filePath(QStringLiteral("unavailable")), "validate");
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        dialog.show();
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        save->click();
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        QVERIFY(!dialog.result().saved);
        QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("saved"))));
        QFile calls(directory.filePath(QStringLiteral("calls")));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QVERIFY(!calls.readAll().split('\n').contains(QByteArray("set")));
    }
    void deleteUsesOneRequestAndReadback_data()
    {
        QTest::addColumn<bool>("timeout");
        QTest::newRow("confirmed") << false;
        QTest::newRow("uncertain then reload") << true;
    }
    void deleteUsesOneRequestAndReadback()
    {
        QFETCH(bool, timeout);
        QTemporaryDir directory;
        const auto executable = backend(directory);
        QVERIFY(!executable.isEmpty());
        if (timeout) textFile(directory.filePath(QStringLiteral("delete-mode")), "timeout");
        const auto original = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", executable.toUtf8());
        const auto restore = qScopeGuard([original] { if (original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", original); });
        dwarfsplugin::ProfileEditorDialog dialog(nullptr, QStringLiteral("fixture"));
        dialog.show();
        auto *save = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 5000);
        QPushButton *remove = nullptr;
        for (auto *button : dialog.findChildren<QPushButton *>()) if (button->text() == QLatin1String("Delete")) remove = button;
        QVERIFY(remove);
        QTimer::singleShot(0, &dialog, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *question = qobject_cast<QMessageBox *>(widget)) question->button(QMessageBox::Yes)->click();
        });
        remove->click();
        auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("profileName"));
        if (timeout) {
            auto *retry = dialog.findChild<QPushButton *>(QStringLiteral("profileRetry"));
            QTRY_VERIFY_WITH_TIMEOUT(retry->isVisible() && retry->isEnabled(), 8000);
            QVERIFY(!save->isEnabled());
            QCOMPARE(name->text(), QStringLiteral("fixture"));
            retry->click();
        }
        QTRY_VERIFY_WITH_TIMEOUT(name->text().isEmpty(), 5000);
        QVERIFY(save->isEnabled());
        QFile calls(directory.filePath(QStringLiteral("calls")));
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll().split('\n').count(QByteArray("delete")), 1);
    }
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
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QLabel *>(QStringLiteral("profileLoadStatus"))->text() == QLatin1String("Ready"), 5000);
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
