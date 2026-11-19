// SPDX-License-Identifier: GPL-3.0-or-later
#include "directmountoperation.h"
#include "operationstatus.h"
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <QtTest>
#include <KLocalizedString>
#include <atomic>
#include <memory>

class DirectMountTest final : public QObject {
    Q_OBJECT
    QDialog *feedback()
    {
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->objectName() == QLatin1String("directMountFeedback")) return qobject_cast<QDialog *>(widget);
        return nullptr;
    }
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void workerRemainsResponsiveAndCannotOverlapOrCancel()
    {
        dwarfsctl_tray::DirectMountOperation operation;
        const auto gate = std::make_shared<QSemaphore>();
        const auto started = std::make_shared<std::atomic<bool>>(false);
        QSignalSpy result(&operation, &dwarfsctl_tray::DirectMountOperation::finished);
        QVERIFY(operation.start(QStringLiteral("Mount"), QStringLiteral("Mounting…"), QStringLiteral("Mounted"), [gate, started] {
            started->store(true);
            gate->acquire();
            return QByteArray(R"({"ok":true})");
        }));
        // Always release the worker even if a following assertion fails.
        const auto release = qScopeGuard([gate] { gate->release(); });
        QTRY_VERIFY(started->load());
        bool tick = false;
        QTimer::singleShot(0, this, [&tick] { tick = true; });
        QTRY_VERIFY(tick);
        QVERIFY(operation.isActive());
        bool secondRan = false;
        QVERIFY(!operation.start(QStringLiteral("Other"), {}, {}, [&secondRan] { secondRan = true; return QByteArray{}; }));
        QVERIFY(!secondRan);
        auto *dialog = feedback();
        QVERIFY(dialog);
        auto *close = dialog->findChild<QPushButton *>(QStringLiteral("closeMountOperation"));
        QVERIFY(close && !close->isEnabled());
        QCOMPARE(dialog->findChildren<QPushButton *>().size(), 1); // No fake Cancel.
        dialog->close();
        QVERIFY(dialog->isVisible());
        QCOMPARE(result.count(), 0);
        gate->release();
        QTRY_COMPARE(result.count(), 1);
        QVERIFY(!operation.isActive());
        QVERIFY(close->isEnabled());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("operationStatusText"))->text(), QStringLiteral("Mounted"));
        dialog->close();
    }
    void failuresAndInvalidResponsesStayInTheSamePlainTextSurface_data()
    {
        QTest::addColumn<QByteArray>("payload");
        QTest::addColumn<QString>("text");
        QTest::newRow("literal error") << QByteArray(R"({"ok":false,"error":{"message":"<b>literal & failure</b>"}})") << QStringLiteral("<b>literal & failure</b>");
        QTest::newRow("invalid response") << QByteArray("not json") << QString();
    }
    void failuresAndInvalidResponsesStayInTheSamePlainTextSurface()
    {
        QFETCH(QByteArray, payload); QFETCH(QString, text);
        dwarfsctl_tray::DirectMountOperation operation;
        QSignalSpy result(&operation, &dwarfsctl_tray::DirectMountOperation::finished);
        QVERIFY(operation.start(QStringLiteral("Mount"), QStringLiteral("Mounting…"), QStringLiteral("Mounted"), [payload] { return payload; }));
        auto *dialog = feedback(); QVERIFY(dialog);
        QTRY_COMPARE(result.count(), 1);
        auto *label = dialog->findChild<QLabel *>(QStringLiteral("operationStatusText"));
        QVERIFY(label); QCOMPARE(label->textFormat(), Qt::PlainText);
        if (!text.isEmpty()) QCOMPARE(label->text(), text);
        else QVERIFY(label->text().contains(QStringLiteral("Check the current mount state")));
        QVERIFY(dialog->isVisible());
        QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("closeMountOperation"))->isEnabled());
        dialog->close();
    }
    void destructionJoinsOwnedWorkInsteadOfTerminatingIt()
    {
        auto done = std::make_shared<std::atomic<bool>>(false);
        {
            dwarfsctl_tray::DirectMountOperation operation;
            QVERIFY(operation.start(QStringLiteral("Mount"), {}, {}, [done] {
                QThread::msleep(30);
                done->store(true);
                return QByteArray(R"({"ok":true})");
            }));
        }
        QVERIFY(done->load());
    }
};
QTEST_MAIN(DirectMountTest)
#include "tst_direct_mount.moc"
