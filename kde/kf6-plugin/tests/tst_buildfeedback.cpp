#include "buildfeedback.h"
#include <QtTest>
#include <QLabel>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
using namespace dwarfsplugin;
class FeedbackTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void progressAndSummaryRemainVisible() {
        QPointer<BuildFeedback> window = new BuildFeedback(QStringLiteral("/tmp/example.dwarfs"));
        QVERIFY(window->isVisible());
        QVERIFY(!window->close());
        window->phase(QStringLiteral("compressing"));
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("Compressing")));
        Envelope env;
        env.ok = true;
        env.details.insert(QStringLiteral("summary"), QJsonObject{{QStringLiteral("files"), 3}, {QStringLiteral("original_bytes"), 4096}, {QStringLiteral("image_bytes"), 1024}, {QStringLiteral("elapsed_ms"), 2300}});
        window->complete(env);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildSummary"))->text().contains(QStringLiteral("example.dwarfs")));
        QCOMPARE(window->findChild<QProgressBar *>()->value(), 100);
        QVERIFY(window->isVisible());
        QCoreApplication::processEvents();
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS")) window->grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/build-summary.png"));
        QVERIFY(window->close());
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
        window->failed();
    }
    void previewDoesNotClaimAnImageWasCreated() {
        auto *window = new BuildFeedback(QStringLiteral("/tmp/not-created.dwarfs"));
        Envelope env; env.ok = true;
        window->complete(env);
        QVERIFY(window->findChild<QLabel *>(QStringLiteral("buildStatus"))->text().contains(QStringLiteral("no image was created")));
        window->close();
    }
};
QTEST_MAIN(FeedbackTest)
#include "tst_buildfeedback.moc"
