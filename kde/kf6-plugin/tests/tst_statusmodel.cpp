// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctljson.h"
#include "dwarfsctlmodel.h"

#include <QTest>

using namespace dwarfsplugin;

class StatusModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void decodesRichImageAndMountMetadata()
    {
        const QByteArray json = R"JSON({
          "ok":true,"command":"status","image":"/tmp/example.dwarfs",
          "details":{
            "mounted":true,"mountpoint":"/run/user/1000/dwarfs/example",
            "mount_fs_type":"fuse.dwarfs","mount_source":"example.dwarfs",
            "tools":{"dwarfsck":{"json":{
              "created_by":"libdwarfs v0.15.7","created_on":"2026-10-05T12:00:00",
              "inode_count":5,"original_filesystem_size":4096,
              "block_count":2,"block_size":1048576,"features":["history"],
              "version":{"header":2,"major":2,"minor":5},
              "root":{"type":"directory","inodes":[
                {"type":"file","name":"one"},
                {"type":"directory","name":"sub","inodes":[{"type":"file","name":"two"}]}
              ]}
            }}}
          },"warnings":[],"warning_objects":[],"error":null
        })JSON";
        const auto parsed = parseEnvelope(json);
        QVERIFY(parsed.parsed);
        StatusDetails details;
        QVERIFY(decodeStatus(parsed.envelope, &details).ok);
        QVERIFY(details.mounted);
        QVERIFY(details.image_metadata_present);
        QCOMPARE(details.created_by, QStringLiteral("libdwarfs v0.15.7"));
        QCOMPARE(details.format_version, QStringLiteral("2.5 (header 2)"));
        QCOMPARE(details.file_count, 2);
        QCOMPARE(details.directory_count, 1);
        QCOMPARE(details.original_size, 4096);
        QCOMPARE(details.block_count, 2);
        QCOMPARE(details.features, QStringList{QStringLiteral("history")});
    }

    void unavailableAuthorityDoesNotReusePreviousMountKnowledge()
    {
        StatusDetails details;
        details.mounted_present = true;
        details.mounted = true;
        details.mountpoint = QStringLiteral("/previous");
        const auto parsed = parseEnvelope(QByteArrayLiteral(
            R"({"ok":true,"details":{"mount_state_known":false,"mounted":true,"mountpoint":"/stale"}})"));
        QVERIFY(parsed.parsed);
        QVERIFY(decodeStatus(parsed.envelope, &details).ok);
        QVERIFY(!details.mounted_present);
        QVERIFY(!details.mounted);
        QVERIFY(details.mountpoint.isEmpty());
    }

    void acceptsLightweightMountStatus()
    {
        const auto parsed = parseEnvelope(QByteArrayLiteral(
            R"({"ok":true,"command":"mount_status","image":"/tmp/example.dwarfs","details":{"mounted":false,"mountpoint":null},"warnings":[],"warning_objects":[],"error":null})"));
        QVERIFY(parsed.parsed);
        StatusDetails details;
        QVERIFY(decodeStatus(parsed.envelope, &details).ok);
        QVERIFY(details.mounted_present);
        QVERIFY(!details.mounted);
        QVERIFY(!details.image_metadata_present);
        QCOMPARE(details.file_count, -1);
    }
};

QTEST_MAIN(StatusModelTest)
#include "tst_statusmodel.moc"
