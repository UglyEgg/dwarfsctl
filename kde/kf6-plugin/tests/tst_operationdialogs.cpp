// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "checksumdialog.h"
#include "detailsdialog.h"
#include "integritycheckdialog.h"
#include "operationstatus.h"
#include <KLocalizedString>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <memory>

using dwarfsctl_kde::OperationStatus;
using namespace dwarfsplugin;

class OperationDialogsTest final : public QObject {
    Q_OBJECT
    QTemporaryDir m_directory;
    QByteArray m_original;
    QString path(const QString &name) const { return m_directory.filePath(name); }
    void mode(const QByteArray &value)
    {
        QFile file(path(QStringLiteral("mode")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(value), value.size());
        QFile::remove(path(QStringLiteral("pids")));
        QFile::remove(path(QStringLiteral("calls")));
    }
    std::unique_ptr<QDialog> dialog(int operation)
    {
        if (!operation) return std::make_unique<IntegrityCheckDialog>(QStringLiteral("/test/image.dwarfs"));
        auto result = std::make_unique<ChecksumDialog>(QStringLiteral("/test/image.dwarfs"));
        result->findChild<QLineEdit *>(QStringLiteral("checksumVerifyInput"))->setText(QStringLiteral("/test/input.checksums"));
        result->findChild<QLineEdit *>(QStringLiteral("checksumExportPath"))->setText(path(QStringLiteral("output.checksums")));
        return result;
    }
    QPushButton *button(QDialog *dialog, const QString &name) { return dialog->findChild<QPushButton *>(name); }
    QPushButton *startButton(QDialog *dialog, int operation)
    {
        return button(dialog, operation == 0 ? QStringLiteral("integrityRunButton")
            : operation == 1 ? QStringLiteral("checksumExportButton") : QStringLiteral("checksumVerifyButton"));
    }
    OperationStatus *status(QDialog *dialog)
    {
        return static_cast<OperationStatus *>(dialog->findChild<QWidget *>(QStringLiteral("operationStatus")));
    }
    QString prefix(int operation) { return operation ? QStringLiteral("checksum") : QStringLiteral("integrity"); }
    void watchDialogs(QTimer &timer, QDialog *own, bool &duplicate)
    {
        connect(&timer, &QTimer::timeout, &timer, [own, &duplicate] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (widget != own && widget->isVisible()) {
                    if (auto *dialog = qobject_cast<QDialog *>(widget)) {
                        duplicate = true;
                        dialog->reject();
                    }
                }
            }
        });
        timer.start(10);
    }
    void checkControls(QDialog *dialog, bool enabled)
    {
        for (const auto &name : {"checksumAlgorithm", "checksumExportPath", "checksumVerifyInput", "checksumVerifyDirectory",
                 "checksumExportBrowseButton", "checksumInputBrowseButton", "checksumDirectoryBrowseButton"}) {
            auto *widget = dialog->findChild<QWidget *>(QString::fromLatin1(name));
            QVERIFY(widget);
            QCOMPARE(widget->isEnabled(), enabled);
        }
    }
    QJsonArray ownedProcesses()
    {
        QFile pids(path(QStringLiteral("pids")));
        if (!pids.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(pids.readAll()).array();
    }
    bool allStopped(const QJsonArray &processes)
    {
        for (const auto &value : processes) {
            const auto record = value.toArray();
            QFile file(QStringLiteral("/proc/%1/stat").arg(record.at(0).toInteger()));
            if (!file.open(QIODevice::ReadOnly)) continue;
            const auto raw = file.readAll();
            const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
            if (fields.size() > 19 && fields.at(19).toLongLong() == record.at(1).toInteger() && fields.at(0) != "Z") return false;
        }
        return true;
    }
    int invocationCount()
    {
        QFile file(path(QStringLiteral("calls")));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        return file.readAll().count('\n');
    }
private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("dwarfsctl");
        QVERIFY(m_directory.isValid());
        m_original = qgetenv("DWARFSCTL_BIN");
        QFile backend(path(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        backend.write(R"PY(#!/usr/bin/python3
import json,sys,time,os,subprocess
from pathlib import Path
root=Path(sys.argv[0]).parent
with (root/'calls').open('a') as log: log.write(json.dumps(sys.argv[1:])+'\n')
mode=(root/'mode').read_text()
if mode=='hold':
    child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])
    def identity(pid):
        raw=Path('/proc/'+str(pid)+'/stat').read_text()
        return [pid,int(raw[raw.rfind(')')+2:].split()[19])]
    (root/'pids').write_text(json.dumps([identity(os.getpid()),identity(child.pid)]))
    time.sleep(30)
if mode=='fail':
    print(json.dumps(dict(ok=False,command=sys.argv[1],details={},error=dict(code=9,kind='Reader',message='<b>failed & literal</b>'))))
    sys.exit(9)
print(json.dumps(dict(ok=True,command=sys.argv[1],details={})))
)PY");
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());
    }
    void cleanupTestCase()
    {
        if (m_original.isNull()) qunsetenv("DWARFSCTL_BIN"); else qputenv("DWARFSCTL_BIN", m_original);
    }
    void detailsOnlyShowsAvailableSections()
    {
        Envelope clean;
        clean.ok = true;
        clean.command = QStringLiteral("fsck");
        clean.image = QStringLiteral("/test/image.dwarfs");
        DetailsDialog cleanDialog(clean, QStringLiteral("Details"));
        auto *tabs = cleanDialog.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0), QStringLiteral("Summary"));
        QCOMPARE(tabs->tabText(1), QStringLiteral("Raw"));
        for (auto *label : cleanDialog.findChildren<QLabel *>())
            QVERIFY(label->text() != QStringLiteral("(none)"));

        clean.ok = false;
        clean.has_error = true;
        clean.error = {9, {}, QStringLiteral("Image could not be read")};
        clean.warnings.append(QString());
        clean.warning_objects.append(WarningInfo{});
        clean.details.insert(QStringLiteral("tool"), QJsonObject{{QStringLiteral("stdout"), QString()}});
        DetailsDialog errorDialog(clean, QStringLiteral("Details"));
        tabs = errorDialog.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Error"));

        clean.warnings.append(QStringLiteral("Metadata warning"));
        clean.details.insert(QStringLiteral("tool"), QJsonObject{
            {QStringLiteral("json"), QJsonObject{{QStringLiteral("block_count"), 1}}}});
        DetailsDialog fullDialog(clean, QStringLiteral("Details"));
        tabs = fullDialog.findChild<QTabWidget *>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 5);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Warnings"));
        QCOMPARE(tabs->tabText(2), QStringLiteral("Error"));
        QCOMPARE(tabs->tabText(3), QStringLiteral("Tools"));
    }
    void successFailureAndRetry_data()
    {
        QTest::addColumn<int>("operation");
        QTest::newRow("integrity") << 0;
        QTest::newRow("export") << 1;
        QTest::newRow("verify") << 2;
    }
    void successFailureAndRetry()
    {
        QFETCH(int, operation);
        mode("fail");
        auto own = dialog(operation);
        bool duplicate = false;
        QTimer watchdog;
        watchDialogs(watchdog, own.get(), duplicate);
        own->show();
        auto *run = startButton(own.get(), operation);
        QVERIFY(run);
        if (!operation) {
            QVERIFY(!status(own.get())->isVisible());
            QVERIFY(!button(own.get(), QStringLiteral("integrityDetailsButton"))->isVisible());
            QCOMPARE(own->findChild<QLabel *>(QStringLiteral("integrityImageName"))->text(),
                QStringLiteral("image.dwarfs"));
        }
        run->click();
        QTRY_VERIFY_WITH_TIMEOUT(status(own.get())->state() == OperationStatus::State::Failed, 5000);
        QVERIFY(!duplicate);
        auto *text = own->findChild<QLabel *>(prefix(operation) + QStringLiteral("StatusText"));
        QVERIFY(text);
        QCOMPARE(text->textFormat(), Qt::PlainText);
        const QString expected = operation == 0
            ? QStringLiteral("Integrity check failed.")
            : operation == 1 ? QStringLiteral("The checksums could not be exported. Open Details for more information.")
                             : QStringLiteral("The checksums could not be verified. Open Details for more information.");
        QCOMPARE(text->text(), expected);
        auto *details = button(own.get(), prefix(operation) + QStringLiteral("DetailsButton"));
        QVERIFY(details->isEnabled());
        if (!operation) {
            QVERIFY(details->isVisible());
            auto *reason = own->findChild<QLabel *>(QStringLiteral("integrityResultDetail"));
            QVERIFY(reason->isVisible());
            QCOMPARE(reason->text(), QStringLiteral("<b>failed & literal</b>"));
            QCOMPARE(reason->textFormat(), Qt::PlainText);
        }
        // Details is explicitly requested. Inspect its read-only literal payload,
        // then close it from a bounded timer so failure cannot hang the test.
        watchdog.stop();
        bool detailsOpened = false;
        bool diagnosticPreserved = false;
        bool literalLabelPreserved = false;
        QTimer detailCloser;
        connect(&detailCloser, &QTimer::timeout, &detailCloser, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                auto *childDialog = qobject_cast<QDialog *>(widget);
                if (!childDialog || childDialog == own.get() || !childDialog->isVisible()) continue;
                detailsOpened = true;
                for (auto *label : childDialog->findChildren<QLabel *>()) {
                    if (label->text().contains(QStringLiteral("<b>failed & literal</b>")) && label->textFormat() == Qt::PlainText)
                        literalLabelPreserved = true;
                }
                for (auto *editor : childDialog->findChildren<QPlainTextEdit *>()) {
                    if (editor->isReadOnly() && editor->toPlainText().contains(QStringLiteral("<b>failed & literal</b>")))
                        diagnosticPreserved = true;
                }
                childDialog->reject();
            }
        });
        detailCloser.start(10);
        details->click();
        detailCloser.stop();
        QVERIFY(detailsOpened);
        QVERIFY(diagnosticPreserved);
        QVERIFY(literalLabelPreserved);
        watchdog.start(10);
        QVERIFY(run->isEnabled());
        if (operation) checkControls(own.get(), true);
        mode("success");
        run->click();
        QTRY_VERIFY_WITH_TIMEOUT(status(own.get())->state() == OperationStatus::State::Succeeded, 5000);
        QVERIFY(!duplicate);
        QCOMPARE(invocationCount(), 1);
        QVERIFY(run->isEnabled());
        if (!operation) {
            QVERIFY(!details->isVisible());
            QVERIFY(!own->findChild<QLabel *>(QStringLiteral("integrityResultDetail"))->isVisible());
        }
        if (operation) {
            auto *field = own->findChild<QLineEdit *>(operation == 1
                ? QStringLiteral("checksumExportPath") : QStringLiteral("checksumVerifyInput"));
            field->clear();
            run->click();
            QVERIFY(status(own.get())->state() == OperationStatus::State::Failed);
            QVERIFY(!button(own.get(), QStringLiteral("checksumDetailsButton"))->isEnabled());
            QCOMPARE(invocationCount(), 1); // Local validation never starts another job.
        }
    }
    void cancelAndRetry_data() { successFailureAndRetry_data(); }
    void cancelAndRetry()
    {
        QFETCH(int, operation);
        mode("hold");
        auto own = dialog(operation);
        bool duplicate = false;
        QTimer watchdog;
        watchDialogs(watchdog, own.get(), duplicate);
        QSignalSpy cancellation(own.get(), SIGNAL(operationCancelRequested()));
        QVERIFY(cancellation.isValid());
        own->show();
        auto *run = startButton(own.get(), operation);
        run->click();
        QVERIFY(!run->isEnabled());
        if (operation) checkControls(own.get(), false);
        QTRY_VERIFY_WITH_TIMEOUT(ownedProcesses().size() == 2, 5000);
        const auto processes = ownedProcesses();
        QCOMPARE(invocationCount(), 1);
        run->click(); // A repeated start cannot create another operation.
        QCOMPARE(invocationCount(), 1);
        auto *cancel = button(own.get(), prefix(operation) + QStringLiteral("CancelButton"));
        QVERIFY(cancel && cancel->isEnabled());
        cancel->click();
        cancel->click();
        QCOMPARE(cancellation.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(status(own.get())->state() == OperationStatus::State::Cancelled, 5000);
        QVERIFY(own->isVisible());
        QVERIFY(allStopped(processes));
        QVERIFY(!duplicate);
        QVERIFY(run->isEnabled());
        if (operation) checkControls(own.get(), true);
        mode("success");
        run->click();
        QTRY_VERIFY_WITH_TIMEOUT(status(own.get())->state() == OperationStatus::State::Succeeded, 5000);
        QVERIFY(!duplicate);
        QCOMPARE(cancellation.count(), 1);
    }
    void closeWaitsForCleanup_data()
    {
        QTest::addColumn<int>("operation");
        QTest::addColumn<int>("intent");
        for (int operation : {0, 1}) {
            for (int intent : {0, 1, 2})
                QTest::newRow(qPrintable(QStringLiteral("%1-close-%2").arg(operation).arg(intent))) << operation << intent;
        }
    }
    void closeWaitsForCleanup()
    {
        QFETCH(int, operation);
        QFETCH(int, intent);
        mode("hold");
        auto own = dialog(operation);
        QSignalSpy cancellation(own.get(), SIGNAL(operationCancelRequested()));
        own->show();
        startButton(own.get(), operation)->click();
        QTRY_VERIFY_WITH_TIMEOUT(ownedProcesses().size() == 2, 5000);
        const auto processes = ownedProcesses();
        if (intent == 0) QTest::keyClick(own.get(), Qt::Key_Escape);
        else if (intent == 1) own->close();
        else button(own.get(), prefix(operation) + QStringLiteral("CloseButton"))->click();
        own->reject(); // Another close intent during cancellation remains idempotent.
        QCOMPARE(cancellation.count(), 1);
        QVERIFY(own->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!own->isVisible(), 5000);
        QVERIFY(allStopped(processes));
        QVERIFY(status(own.get())->state() == OperationStatus::State::Cancelled);
    }
};
QTEST_MAIN(OperationDialogsTest)
#include "tst_operationdialogs.moc"
