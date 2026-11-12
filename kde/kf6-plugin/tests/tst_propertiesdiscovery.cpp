// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KFileItem>
#include <KPropertiesDialog>

#include <QFile>
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
            "printf '%s\\n' '{\"ok\":true,\"command\":\"status\",\"image\":\"sample.dwarfs\","
            "\"details\":{\"mounted\":false,\"mountpoint\":null},\"warnings\":[],"
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
        QTest::qWait(100);
        if (originalBackend.isNull()) {
            qunsetenv("DWARFSCTL_BIN");
        } else {
            qputenv("DWARFSCTL_BIN", originalBackend);
        }
    }
};

QTEST_MAIN(PropertiesDiscoveryTest)
#include "tst_propertiesdiscovery.moc"
