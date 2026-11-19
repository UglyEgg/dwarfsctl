// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KFileItem>
#include <KPropertiesDialog>

#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

class PropertiesDiscoveryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void exposesDwarfsImagePage()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        const QString path = directory.filePath(QStringLiteral("sample.dwarfs"));
        QFile image(path);
        QVERIFY(image.open(QIODevice::WriteOnly));
        QCOMPARE(image.write("DWARFS"), 6);
        image.close();

        QFile backend(directory.filePath(QStringLiteral("dwarfsctl")));
        QVERIFY(backend.open(QIODevice::WriteOnly));
        const QByteArray response =
            "#!/bin/sh\n"
            "if [ \"$1\" = config ]; then printf '%s\\n' '{\"ok\":true,\"details\":{\"command_timeout_ms\":300000,\"build_timeout_ms\":0,\"open_after_mount\":true}}'; exit; fi\n"
            "printf '%s\\n' '{\"ok\":true,\"command\":\"status\",\"image\":\"sample.dwarfs\","
            "\"details\":{\"mounted\":false,\"mountpoint\":null,\"tools\":{\"dwarfsck\":{\"json\":{\"created_by\":\"<b>crafted creator</b>\"}}}},\"warnings\":[],"
            "\"warning_objects\":[],\"error\":null}'\n";
        QCOMPARE(backend.write(response), response.size());
        backend.close();
        QVERIFY(backend.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray originalBackend = qgetenv("DWARFSCTL_BIN");
        qputenv("DWARFSCTL_BIN", backend.fileName().toUtf8());

        const KFileItem item(QUrl::fromLocalFile(path), KFileItem::SkipMimeTypeFromContent);
        QCOMPARE(item.mimetype(), QStringLiteral("application/x-dwarfs"));
        KPropertiesDialog dialog(item, nullptr);

        QVERIFY2(dialog.findChild<QWidget *>(QStringLiteral("dwarfsImagePropertiesPage")),
                 "KF6 did not instantiate the DwarFS Image Properties page");
        auto *creator = dialog.findChild<QLabel *>(QStringLiteral("imageCreatedBy"));
        QVERIFY(creator);
        QTRY_COMPARE_WITH_TIMEOUT(creator->text(), QStringLiteral("<b>crafted creator</b>"), 5000);
        QCOMPARE(creator->textFormat(), Qt::PlainText);
        auto *mountpoint = dialog.findChild<QLabel *>(QStringLiteral("imageMountpoint"));
        QVERIFY(mountpoint);
        QCOMPARE(mountpoint->textFormat(), Qt::PlainText);
        if (originalBackend.isNull()) {
            qunsetenv("DWARFSCTL_BIN");
        } else {
            qputenv("DWARFSCTL_BIN", originalBackend);
        }
    }
};

QTEST_MAIN(PropertiesDiscoveryTest)
#include "tst_propertiesdiscovery.moc"
