#include "buildfeedback.h"
#include <QtTest>
#include <QLabel>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
using namespace dwarfsplugin;
class FeedbackTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void progressAndSummaryRemainVisible() {
        QTemporaryDir directory;
        QFile output(directory.filePath(QStringLiteral("example.dwarfs")));
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(QByteArray(1024, 'x'));
        output.close();
        QPointer<BuildFeedback> window = new BuildFeedback(output.fileName());
        QVERIFY(window->isVisible());
        window->phase(QStringLiteral("compressing"));
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("Compressing")));
        Envelope env;
        env.ok = true;
        env.details.insert(QStringLiteral("summary"), QJsonObject{{QStringLiteral("files"), 3}, {QStringLiteral("original_bytes"), 4096}, {QStringLiteral("image_bytes"), 1024}, {QStringLiteral("elapsed_ms"), 2300}});
        window->complete(env);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildSummary"))->text().contains(QStringLiteral("example.dwarfs")));
        QCOMPARE(window->findChild<QProgressBar *>(QStringLiteral("buildSavingsBar"))->value(), 75);
        QVERIFY(window->isVisible());
        QCoreApplication::processEvents();
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS")) window->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-summary.png"));
        QVERIFY(window->close());
    }
    void creationRequiresARegularOutputFile_data() {
        QTest::addColumn<QString>("kind");
        QTest::newRow("missing") << QStringLiteral("missing");
        QTest::newRow("directory") << QStringLiteral("directory");
        QTest::newRow("symlink") << QStringLiteral("symlink");
    }
    void creationRequiresARegularOutputFile() {
        QFETCH(QString, kind);
        QTemporaryDir directory;
        const auto output = directory.filePath(QStringLiteral("image.dwarfs"));
        if (kind == QLatin1String("directory")) QVERIFY(QDir().mkdir(output));
        if (kind == QLatin1String("symlink")) {
            QFile target(directory.filePath(QStringLiteral("actual.dwarfs")));
            QVERIFY(target.open(QIODevice::WriteOnly)); target.write("image"); target.close();
            QVERIFY(QFile::link(target.fileName(), output));
        }
        QPointer<BuildFeedback> window = new BuildFeedback(output);
        Envelope env; env.ok = true;
        env.details.insert(QStringLiteral("summary"), QJsonObject{{QStringLiteral("files"), 1}});
        window->complete(env);
        QCOMPARE(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text(), QStringLiteral("Could Not Create Image"));
        QVERIFY(!window->findChild<QPushButton *>(QStringLiteral("openBuildFolder"))->isEnabled());
        window->close();
        QTRY_VERIFY(window.isNull());
    }
    void cancellationIsRequestedOnceAndKeepsTheWindowUntilTheJobStops() {
        auto *window = new BuildFeedback(QStringLiteral("/tmp/cancel.dwarfs"));
        QSignalSpy cancelled(window, &BuildFeedback::cancelRequested);
        auto *button = window->findChild<QPushButton *>(QStringLiteral("cancelBuild"));
        QVERIFY(button);
        QVERIFY(button->isEnabled());
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS")) {
            window->resize(480, window->sizeHint().height());
            window->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-cancel-ready.png"));
        }
        button->click();
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS"))
            window->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-cancelling.png"));
        QCOMPARE(cancelled.count(), 1);
        QVERIFY(!button->isEnabled());
        QVERIFY(window->isVisible());
        window->phase(QStringLiteral("compressing"));
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("Cancelling")));
        QVERIFY(!window->close());
        QCOMPARE(cancelled.count(), 1);
        window->cancelled();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void failureStaysInTheOwningWindowAndCloseWaitsForCleanup() {
        QPointer<BuildFeedback> window = new BuildFeedback(QStringLiteral("/tmp/<image>.dwarfs"));
        window->failed(QStringLiteral("<b>cannot write</b>"));
        QVERIFY(window->isVisible());
        auto *summary = window->findChild<QLabel *>(QStringLiteral("buildSummary"));
        QCOMPARE(summary->textFormat(), Qt::PlainText);
        QCOMPARE(summary->text(), QStringLiteral("<b>cannot write</b>"));
        window->complete(Envelope{}); // A late result cannot replace the failure.
        QCOMPARE(summary->text(), QStringLiteral("<b>cannot write</b>"));
        window->close();
        QTRY_VERIFY(window.isNull());
        window = new BuildFeedback(QStringLiteral("/tmp/cancel.dwarfs"));
        QSignalSpy requested(window, &BuildFeedback::cancelRequested);
        window->close();
        QCOMPARE(requested.count(), 1);
        QVERIFY(window->isVisible());
        window->cancelled();
        QTRY_VERIFY(window.isNull());
    }
    void cancelButtonLeavesAnHonestTerminalResult() {
        QPointer<BuildFeedback> window = new BuildFeedback(QStringLiteral("/tmp/cancel.dwarfs"));
        window->findChild<QPushButton *>(QStringLiteral("cancelBuild"))->click();
        window->cancelled();
        QVERIFY(window->isVisible());
        QCOMPARE(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text(), QStringLiteral("Cancelled"));
        QVERIFY(!window->findChild<QPushButton *>(QStringLiteral("openBuildFolder"))->isEnabled());
        window->close();
        QTRY_VERIFY(window.isNull());
    }
    void closeIntentSurvivesNaturalCompletionOrFailure() {
        for (const bool failure : {false, true}) {
            QPointer<BuildFeedback> window = new BuildFeedback(QStringLiteral("/tmp/closed.dwarfs"));
            window->close();
            QVERIFY(window->isVisible());
            if (failure) window->failed(QStringLiteral("write failed"));
            else window->complete(Envelope{});
            QTRY_VERIFY(window.isNull());
        }
    }
    void previewShowsMeasuredSizesWithoutClaimingPublication() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("not-created.dwarfs"));
        auto *window = new BuildFeedback(path, nullptr, true);
        const auto stage = window->previewStage();
        QVERIFY(!stage.isEmpty());
        const QString candidate = stage + QStringLiteral("/candidate.dwarfs");
        QFile staged(candidate);
        QVERIFY(staged.open(QIODevice::WriteOnly));
        QCOMPARE(staged.write(QByteArray(1024, 'x')), qint64(1024));
        staged.close();
        Envelope env; env.ok = true;
        env.details.insert(QStringLiteral("preview_candidate"), candidate);
        env.details.insert(QStringLiteral("summary"), QJsonObject{
            {QStringLiteral("preview"), true}, {QStringLiteral("files"), 3},
            {QStringLiteral("original_bytes"), 4096}, {QStringLiteral("image_bytes"), 1024},
            {QStringLiteral("source_types"), QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral(".txt")}, {QStringLiteral("files"), 3},
                {QStringLiteral("original_bytes"), 4096}}}}});
        window->complete(env);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("no image was created")));
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildImageSize"))->text().contains(QStringLiteral("KiB")));
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildSavedSize"))->text().contains(QStringLiteral("75%")));
        QVERIFY(window->findChild<QWidget *>(QStringLiteral("buildSourceTypes"))->isVisible());
        QVERIFY(!window->findChild<QPushButton *>(QStringLiteral("openBuildFolder"))->isVisible());
        auto *create = window->findChild<QPushButton *>(QStringLiteral("createMeasuredImage"));
        QVERIFY(create && create->isVisible());
        QSignalSpy requested(window, &BuildFeedback::createRequested);
        QVERIFY(!QFileInfo::exists(path));
        QCoreApplication::processEvents();
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS"))
            window->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-preview-measured.png"));
        create->click();
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), candidate);
        QCOMPARE(requested.at(0).at(1).toLongLong(), qint64(1024));
        QVERIFY(!window->close());
        QFile output(path);
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(QByteArray(1024, 'x')), qint64(1024));
        output.close();
        Envelope published;
        published.ok = true;
        published.details.insert(QStringLiteral("published_preview"), true);
        published.details.insert(QStringLiteral("output_verified"), true);
        published.details.insert(QStringLiteral("image_bytes"), 1024);
        window->published(published);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("created successfully")));
        QVERIFY(!QFileInfo::exists(stage));
        window->close();
    }
    void previewRejectsPlanOnlyResult() {
        auto *window = new BuildFeedback(QStringLiteral("/tmp/not-created.dwarfs"), nullptr, true);
        Envelope env; env.ok = true;
        window->complete(env);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("Could Not Measure")));
        window->close();
    }
    void closingPreviewDiscardsTheMeasuredImage() {
        QTemporaryDir directory;
        QPointer<BuildFeedback> window = new BuildFeedback(directory.filePath(QStringLiteral("image.dwarfs")), nullptr, true);
        const QString stage = window->previewStage();
        QVERIFY(!stage.isEmpty());
        const QString candidate = stage + QStringLiteral("/candidate.dwarfs");
        QFile file(candidate);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(32, 'x')), qint64(32));
        file.close();
        Envelope env; env.ok = true;
        env.details.insert(QStringLiteral("preview_candidate"), candidate);
        env.details.insert(QStringLiteral("summary"), QJsonObject{
            {QStringLiteral("preview"), true}, {QStringLiteral("original_bytes"), 64},
            {QStringLiteral("image_bytes"), 32}});
        window->complete(env);
        QVERIFY(QFileInfo::exists(candidate));
        window->close();
        QTRY_VERIFY(window.isNull());
        QVERIFY(!QFileInfo::exists(stage));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("image.dwarfs"))));
    }
};
QTEST_MAIN(FeedbackTest)
#include "tst_buildfeedback.moc"
