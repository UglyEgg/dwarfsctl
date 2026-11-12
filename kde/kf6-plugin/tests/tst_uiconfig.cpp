// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the shared `[ui]` reader.
//
// WHY THIS EXISTS. The Dolphin plugin and the tray used to open
// /etc/dwarfs-dolphin/config.toml and ~/.config/dwarfs-dolphin/config.toml
// themselves and hand-parse the `[ui]` keys. That made each front end a second
// implementation of config precedence, environment expansion and defaults --
// two parsers that would disagree with dwarfsctl the first time any of the
// three changed. They now read the resolved values from
// `dwarfsctl config show --section ui`, and this suite pins the envelope
// contract they rely on: all three keys, right types, Qt timeout convention.
//
// It is pure JSON handling: no backend, no widgets, no /dev/fuse, so it runs
// anywhere including CI.

#include "uiconfig.h"

#include <QJsonDocument>
#include <QTest>

#include <limits>

using namespace dwarfsctl_kde;

namespace {

QByteArray envelope(const QString &details)
{
    return QStringLiteral(
               "{\"ok\": true, \"command\": \"config_show\", \"image\": null, "
               "\"details\": %1, \"warnings\": [], \"warning_objects\": [], "
               "\"error\": null}")
        .arg(details)
        .toUtf8();
}

} // namespace

class UiConfigTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- the happy path -----------------------------------------------------

    void parsesTheResolvedValues()
    {
        UiConfig out;
        QVERIFY(parseUiConfig(envelope(QStringLiteral(
                                  "{\"open_after_mount\": false, "
                                  "\"command_timeout_ms\": 120000, "
                                  "\"build_timeout_ms\": 900000}")),
                              &out));
        QVERIFY(out.from_backend);
        QCOMPARE(out.open_after_mount, false);
        QCOMPARE(out.command_timeout_ms, 120000);
        QCOMPARE(out.build_timeout_ms, 900000);
    }

    /// The config's 0 means "wait indefinitely", which is Qt's -1.
    void zeroBecomesNoTimeout()
    {
        UiConfig out;
        QVERIFY(parseUiConfig(envelope(QStringLiteral(
                                  "{\"open_after_mount\": true, "
                                  "\"command_timeout_ms\": 0, "
                                  "\"build_timeout_ms\": 0}")),
                              &out));
        QCOMPARE(out.command_timeout_ms, -1);
        QCOMPARE(out.build_timeout_ms, -1);
    }

    /// A value past what Qt accepts is clamped, never wrapped negative.
    void hugeValueIsClamped()
    {
        UiConfig out;
        QVERIFY(parseUiConfig(envelope(QStringLiteral(
                                  "{\"open_after_mount\": true, "
                                  "\"command_timeout_ms\": 4294967295, "
                                  "\"build_timeout_ms\": 300000}")),
                              &out));
        QCOMPARE(out.command_timeout_ms, std::numeric_limits<int>::max());
    }

    // --- rejection: a contract violation must not be guessed around ---------

    /// A failed envelope is a failure, even if a details object is present.
    void failedEnvelopeIsRejected()
    {
        const QByteArray json = QStringLiteral(
                                    "{\"ok\": false, \"command\": \"config_show\", "
                                    "\"details\": {\"open_after_mount\": true, "
                                    "\"command_timeout_ms\": 1, \"build_timeout_ms\": 2}, "
                                    "\"error\": {\"code\": 2, \"kind\": \"BadCliUsage\", "
                                    "\"message\": \"unknown section\"}}")
                                    .toUtf8();
        UiConfig out;
        QVERIFY(!parseUiConfig(json, &out));
        QVERIFY(!out.from_backend);
    }

    /// Every key must be present with the declared type. dwarfsctl emits all
    /// three unconditionally, so anything else is a different backend, and
    /// defaulting the missing key would hide that.
    void missingOrMistypedKeysAreRejected()
    {
        const char *details[] = {
            "{\"command_timeout_ms\": 1, \"build_timeout_ms\": 2}",
            "{\"open_after_mount\": true, \"build_timeout_ms\": 2}",
            "{\"open_after_mount\": true, \"command_timeout_ms\": 1}",
            "{\"open_after_mount\": \"true\", \"command_timeout_ms\": 1, \"build_timeout_ms\": 2}",
            "{\"open_after_mount\": true, \"command_timeout_ms\": \"1\", \"build_timeout_ms\": 2}",
            "{\"open_after_mount\": true, \"command_timeout_ms\": 1, \"build_timeout_ms\": null}",
        };
        for (const char *d : details) {
            UiConfig out;
            QVERIFY2(!parseUiConfig(envelope(QString::fromUtf8(d)), &out), d);
            QVERIFY(!out.from_backend);
        }
    }

    void nonObjectPayloadsAreRejected()
    {
        UiConfig out;
        QVERIFY(!parseUiConfig(QByteArray(), &out));
        QVERIFY(!parseUiConfig(QByteArrayLiteral("not json"), &out));
        QVERIFY(!parseUiConfig(QByteArrayLiteral("[]"), &out));
        QVERIFY(!parseUiConfig(QByteArrayLiteral("{\"ok\": true, \"details\": []}"), &out));
        QVERIFY(!out.from_backend);
    }

    /// The defaults are the documented ones, so an unreachable backend leaves
    /// the UI working exactly as a fresh install behaves.
    void defaultsMatchTheDocumentedValues()
    {
        const UiConfig out;
        QVERIFY(!out.from_backend);
        QCOMPARE(out.open_after_mount, true);
        QCOMPARE(out.command_timeout_ms, 300000);
        QCOMPARE(out.build_timeout_ms, -1);
    }
};

QTEST_MAIN(UiConfigTest)
#include "tst_uiconfig.moc"
