// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <limits>

namespace dwarfsctl_kde {

/// The resolved `[ui]` values the KDE front ends consume.
///
/// The front ends do not read the config file. Precedence (user over system),
/// environment expansion and defaults are dwarfsctl's, and this is the one
/// surface they are read through: `dwarfsctl config show --section ui`. A
/// front end that opened the TOML itself would be a second implementation of
/// that resolution, and the two would disagree the first time either changed.
struct UiConfig {
    /// Qt's timeout convention: -1 means "no timeout". The config's 0 ("wait
    /// indefinitely") converts to -1.
    int command_timeout_ms = 300000;
    int build_timeout_ms = -1;

    bool open_after_mount = true;

    /// True when these values came from the backend. False means the backend
    /// could not be asked and the values above are the documented defaults,
    /// so the UI still works without a reachable backend.
    bool from_backend = false;
};

/// Convert a millisecond value as the config carries it into Qt's timeout
/// convention: 0 and below mean "no timeout" (-1), and anything past what Qt
/// accepts is clamped rather than wrapped.
inline int configMsToQtTimeout(qint64 ms)
{
    if (ms <= 0) {
        return -1;
    }
    if (ms > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(ms);
}

/// Parse the envelope of `dwarfsctl config show --section ui` into `*out`.
///
/// Returns true only for a successful envelope carrying all three keys with
/// the types the backend declares. `*out` is untouched on failure, so a
/// caller's existing values survive. Strict on purpose: dwarfsctl emits all
/// three keys unconditionally, so anything else means this is not the backend
/// this front end was written against, and guessing a value would hide that.
inline bool parseUiConfig(const QByteArray &envelope_json, UiConfig *out)
{
    if (!out) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(envelope_json.trimmed());
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject envelope = doc.object();
    if (!envelope.value(QStringLiteral("ok")).toBool(false)) {
        return false;
    }

    const QJsonValue details_value = envelope.value(QStringLiteral("details"));
    if (!details_value.isObject()) {
        return false;
    }
    const QJsonObject details = details_value.toObject();

    const QJsonValue command_value = details.value(QStringLiteral("command_timeout_ms"));
    const QJsonValue build_value = details.value(QStringLiteral("build_timeout_ms"));
    const QJsonValue open_value = details.value(QStringLiteral("open_after_mount"));
    // Numbers arrive as doubles from JSON; a string here is a contract
    // violation, not a value to coerce.
    if (!command_value.isDouble() || !build_value.isDouble() || !open_value.isBool()) {
        return false;
    }

    UiConfig parsed;
    parsed.command_timeout_ms = configMsToQtTimeout(static_cast<qint64>(command_value.toDouble()));
    parsed.build_timeout_ms = configMsToQtTimeout(static_cast<qint64>(build_value.toDouble()));
    parsed.open_after_mount = open_value.toBool();
    parsed.from_backend = true;
    *out = parsed;
    return true;
}

/// Ask the backend for the resolved `[ui]` values.
///
/// `program` is the resolved dwarfsctl to run; callers already resolve it
/// (including the DWARFSCTL_BIN override), so program lookup has one owner.
/// The query is bounded and is made per call rather than cached: a config
/// edit takes effect on the next action instead of requiring a Dolphin or
/// tray restart, which is the behavior these front ends had when they read
/// the file directly. On any failure the defaults above apply with
/// `from_backend == false`.
inline UiConfig loadUiConfig(const QString &program)
{
    UiConfig cfg; // defaults, from_backend == false

    if (program.trimmed().isEmpty()) {
        return cfg;
    }

    QProcess p;
    p.setProgram(program);
    p.setArguments({QStringLiteral("config"),
                    QStringLiteral("show"),
                    QStringLiteral("--section"),
                    QStringLiteral("ui")});
    p.start();
    if (!p.waitForStarted(5000)) {
        return cfg;
    }
    if (!p.waitForFinished(5000)) {
        p.kill();
        p.waitForFinished(1000);
        return cfg;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        return cfg;
    }

    UiConfig parsed;
    if (!parseUiConfig(p.readAllStandardOutput(), &parsed)) {
        return cfg;
    }
    return parsed;
}

} // namespace dwarfsctl_kde
