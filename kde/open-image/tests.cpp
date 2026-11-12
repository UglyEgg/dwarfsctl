#define DWARFSCTL_OPEN_TEST
#include "main.cpp"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
class OpenTest : public QObject {
    Q_OBJECT
    QUrl opened;
private slots:
    void capture(const QUrl &url) { opened = url; }
    void successfulMountOpensReturnedFolder() {
        QTemporaryDir tmp;
        QFile backend(tmp.filePath(QStringLiteral("backend")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        const QByteArray payload = QJsonDocument(QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("details"), QJsonObject{{QStringLiteral("mountpoint"), tmp.path()}}}}).toJson(QJsonDocument::Compact);
        backend.write("#!/bin/sh\nprintf '%s' '" + payload + "'\n");
        backend.close();
        QVERIFY(backend.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
        QDesktopServices::setUrlHandler(QStringLiteral("file"), this, "capture");
        OpenImageWindow window(tmp.filePath(QStringLiteral("image with spaces.dwarfs")), backend.fileName());
        QTRY_COMPARE(opened, QUrl::fromLocalFile(tmp.path()));
        QTRY_VERIFY(!window.isVisible());
        if (!qEnvironmentVariableIsEmpty("DWARFSCTL_TEST_SCREENSHOTS")) window.grab().save(qEnvironmentVariable("DWARFSCTL_TEST_SCREENSHOTS") + QStringLiteral("/mount-success.png"));
        QVERIFY(window.close());
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
    }
    void missingBackendHasVisibleFailure() {
        bool sawError = false;
        QTimer dismiss;
        connect(&dismiss, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *box = qobject_cast<QMessageBox *>(widget)) {
                    sawError = box->icon() == QMessageBox::Critical && !box->text().isEmpty();
                    box->accept();
                }
            }
        });
        dismiss.start(20);
        OpenImageWindow window(QStringLiteral("/missing/image.dwarfs"), QStringLiteral("/no/backend"));
        QTRY_VERIFY(sawError);
        QTRY_VERIFY(!window.isVisible());
    }
};
QTEST_MAIN(OpenTest)
#include "tests.moc"
