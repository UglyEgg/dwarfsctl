// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the shared envelope-text helpers.
//
// WHY THIS EXISTS. The checksum dialog and the integrity dialog each carried a
// private, byte-identical copy of these two helpers (they differed by a single
// comment word). Two copies of one rule drift: a fix applied to one is silently
// absent from the other. They now have one owner (envelopetext.cpp) and this
// suite pins the behavior both dialogs depend on -- which line of a failed
// tool's output a user is shown.

#include "envelopetext.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QTest>

using namespace dwarfsplugin;

namespace {

Envelope envelopeWithTool(const QString &stdoutText, const QString &stderrText)
{
    Envelope env;
    QJsonObject tool;
    tool.insert(QStringLiteral("stdout"), stdoutText);
    tool.insert(QStringLiteral("stderr"), stderrText);
    env.details.insert(QStringLiteral("tool"), tool);
    return env;
}

} // namespace

class EnvelopeTextTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- firstNonEmptyLine -------------------------------------------------

    void firstNonEmptyLineSkipsBlankLines()
    {
        QCOMPARE(firstNonEmptyLine(QStringLiteral("\n  \nhello world\nsecond")),
                 QStringLiteral("hello world"));
    }

    void firstNonEmptyLineTrimsTheLine()
    {
        QCOMPARE(firstNonEmptyLine(QStringLiteral("   padded text   ")),
                 QStringLiteral("padded text"));
    }

    void firstNonEmptyLineOfBlankInputIsEmpty()
    {
        QVERIFY(firstNonEmptyLine(QStringLiteral(" \n\t\n")).isEmpty());
        QVERIFY(firstNonEmptyLine(QString()).isEmpty());
    }

    /// A trailing carriage return must not survive into the user-visible line.
    void firstNonEmptyLineStripsCarriageReturn()
    {
        QCOMPARE(firstNonEmptyLine(QStringLiteral("error text\r\nmore")),
                 QStringLiteral("error text"));
    }

    // --- firstLineFromToolPayload ------------------------------------------

    /// The failure text lives on stderr, so stderr must win when both streams
    /// carry content.
    void stderrIsPreferredOverStdout()
    {
        const Envelope env = envelopeWithTool(QStringLiteral("out line"), QStringLiteral("err line"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("err line"));
    }

    void emptyStderrFallsBackToStdout()
    {
        const Envelope env = envelopeWithTool(QStringLiteral("out line"), QStringLiteral("\n\n"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("out line"));
    }

    /// A tool payload outranks the raw io DwarfsctlJob also attaches.
    void toolPayloadOutranksRawIo()
    {
        Envelope env = envelopeWithTool(QStringLiteral("tool out"), QString());
        env.details.insert(QStringLiteral("_stderr"), QStringLiteral("raw err"));
        env.details.insert(QStringLiteral("_stdout"), QStringLiteral("raw out"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("tool out"));
    }

    /// With no tool object, the raw io is the fallback, stderr first.
    void rawIoIsTheFallbackWhenNoToolObject()
    {
        Envelope env;
        env.details.insert(QStringLiteral("_stderr"), QStringLiteral("raw err"));
        env.details.insert(QStringLiteral("_stdout"), QStringLiteral("raw out"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("raw err"));

        env.details.remove(QStringLiteral("_stderr"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("raw out"));
    }

    /// `details.tool` of the wrong type must be ignored, not crash or win.
    void nonObjectToolValueIsIgnored()
    {
        Envelope env;
        env.details.insert(QStringLiteral("tool"), QStringLiteral("not an object"));
        env.details.insert(QStringLiteral("_stdout"), QStringLiteral("raw out"));
        QCOMPARE(firstLineFromToolPayload(env), QStringLiteral("raw out"));
    }

    void emptyEnvelopeYieldsEmptyLine()
    {
        Envelope env;
        QVERIFY(firstLineFromToolPayload(env).isEmpty());
    }
};

QTEST_MAIN(EnvelopeTextTest)
#include "tst_envelopetext.moc"
