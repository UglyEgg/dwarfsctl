// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "envelopetext.h"

#include <QJsonObject>
#include <QJsonValue>

namespace dwarfsplugin {

QString firstNonEmptyLine(const QString &s)
{
    for (const auto &line : s.split(QLatin1Char('\n'))) {
        const QString t = line.trimmed();
        if (!t.isEmpty()) {
            return t;
        }
    }
    return {};
}

QString firstLineFromToolPayload(const Envelope &env)
{
    // Preferred schema: details.tool.{stdout|stderr}
    const QJsonValue toolV = env.details.value(QStringLiteral("tool"));
    if (toolV.isObject()) {
        const QJsonObject tool = toolV.toObject();
        const QString stderrText = tool.value(QStringLiteral("stderr")).toString();
        const QString stdoutText = tool.value(QStringLiteral("stdout")).toString();
        const QString fromErr = firstNonEmptyLine(stderrText);
        if (!fromErr.isEmpty()) {
            return fromErr;
        }
        const QString fromOut = firstNonEmptyLine(stdoutText);
        if (!fromOut.isEmpty()) {
            return fromOut;
        }
    }

    // Fallback: DwarfsctlJob also attaches raw io under details._stdout/_stderr.
    const QString rawErr = env.details.value(QStringLiteral("_stderr")).toString();
    const QString rawOut = env.details.value(QStringLiteral("_stdout")).toString();
    const QString fromErr = firstNonEmptyLine(rawErr);
    if (!fromErr.isEmpty()) {
        return fromErr;
    }
    return firstNonEmptyLine(rawOut);
}

} // namespace dwarfsplugin
