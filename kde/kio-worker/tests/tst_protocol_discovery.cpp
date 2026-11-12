// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KProtocolInfo>
#include <KProtocolManager>
#include <QTest>
#include <QUrl>

class ProtocolDiscoveryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void installedMetadataIsDiscoverable()
    {
        QVERIFY2(KProtocolInfo::isKnownProtocol(QStringLiteral("dwarfs")),
                 "KF6 could not discover the dwarfs: protocol from plugin metadata");
        QCOMPARE(KProtocolInfo::protocolClass(QStringLiteral("dwarfs")), QStringLiteral(":local"));
        const QUrl url(QStringLiteral("dwarfs:///tmp/example.dwarfs/"));
        QVERIFY(KProtocolManager::supportsListing(url));
        QVERIFY(KProtocolManager::supportsReading(url));
        QVERIFY(!KProtocolManager::supportsWriting(url));
        QVERIFY2(KProtocolManager::canCopyToFile(url),
                 "dwarfs: did not advertise its direct local copy path");
    }
};

QTEST_GUILESS_MAIN(ProtocolDiscoveryTest)

#include "tst_protocol_discovery.moc"
