// SPDX-License-Identifier: GPL-3.0-or-later
#include "extractionfeedback.h"
#include <QDir>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <KLocalizedString>

class ExtractionFeedbackTest final : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain("dwarfsctl"); }
    void closingRequestsOneCancellationAndWaitsForCleanup()
    {
        QPointer<ExtractionFeedback> dialog = new ExtractionFeedback(QStringLiteral("/tmp/example"), true);
        QSignalSpy requested(dialog, &ExtractionFeedback::cancelRequested);
        dialog->show();
        dialog->close();
        QCOMPARE(requested.count(), 1);
        QVERIFY(dialog->isVisible());
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("cancelExtraction"))->isEnabled());
        dialog->close();
        QCOMPARE(requested.count(), 1);
        dialog->cancelled();
        QTRY_VERIFY(dialog.isNull());
    }
    void cancelButtonLeavesAnHonestTerminalResult()
    {
        QPointer<ExtractionFeedback> dialog = new ExtractionFeedback(QStringLiteral("/tmp/cancel"), true);
        dialog->show();
        QSignalSpy requested(dialog, &ExtractionFeedback::cancelRequested);
        dialog->findChild<QPushButton *>(QStringLiteral("cancelExtraction"))->click();
        QCOMPARE(requested.count(), 1);
        dialog->cancelled();
        QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("extractionStatus"))->text(), QStringLiteral("Cancelled"));
        dialog->complete(); // No late success may replace cancellation.
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("extractionStatus"))->text(), QStringLiteral("Cancelled"));
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
    }
    void closeIntentSurvivesNaturalCompletionOrFailure()
    {
        QTemporaryDir destination;
        for (const bool failure : {false, true}) {
            QPointer<ExtractionFeedback> dialog = new ExtractionFeedback(destination.path(), true);
            dialog->show();
            dialog->close();
            QVERIFY(dialog->isVisible());
            if (failure) dialog->failed(QStringLiteral("read failed"));
            else dialog->complete();
            QTRY_VERIFY(dialog.isNull());
        }
    }
    void completedExtractionRequiresARealDirectory()
    {
        QTemporaryDir temp;
        const QString destination = temp.filePath(QStringLiteral("output"));
        QPointer<ExtractionFeedback> dialog = new ExtractionFeedback(destination, true);
        dialog->show();
        dialog->complete();
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("openExtractedFolder"))->isEnabled());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("extractionStatus"))->text(), QStringLiteral("Could Not Extract"));
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(QDir().mkdir(destination));
        dialog = new ExtractionFeedback(destination, false);
        dialog->show();
        dialog->complete();
        QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("openExtractedFolder"))->isEnabled());
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("extractionStatus"))->text(), QStringLiteral("Extraction complete"));
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
    }
    void errorAndDestinationArePlainText()
    {
        QPointer<ExtractionFeedback> dialog = new ExtractionFeedback(QStringLiteral("/tmp/<archive>"), true);
        dialog->show();
        dialog->failed(QStringLiteral("<b>missing & unsafe</b>"));
        const auto *summary = dialog->findChild<QLabel *>(QStringLiteral("extractionSummary"));
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QCOMPARE(summary->text(), QStringLiteral("<b>missing & unsafe</b>"));
        QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("extractionDestination"))->textFormat(), Qt::PlainText);
        QVERIFY(dialog->findChild<QPushButton *>(QStringLiteral("closeExtraction"))->isEnabled());
        QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("openExtractedFolder"))->isEnabled());
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
    }
};
QTEST_MAIN(ExtractionFeedbackTest)
#include "tst_extractionfeedback.moc"
