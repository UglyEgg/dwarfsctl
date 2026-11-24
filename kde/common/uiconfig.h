// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <limits>
#include <cmath>

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

    /// Only validated backend values may enable a configured GUI action.
    /// Default construction is unresolved, not a successful settings load.
    bool from_backend = false;
};

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

    const double command = command_value.toDouble();
    const double build = build_value.toDouble();
    if (!std::isfinite(command) || !std::isfinite(build)
        || std::trunc(command) != command || std::trunc(build) != build
        || command < 0 || build < 0) return false;
    // Clamp before converting: JSON numbers can exceed even qint64.
    const auto timeout = [](double value) {
        return value == 0 ? -1 : value >= std::numeric_limits<int>::max()
            ? std::numeric_limits<int>::max() : static_cast<int>(value);
    };
    UiConfig parsed;
    parsed.command_timeout_ms = timeout(command);
    parsed.build_timeout_ms = timeout(build);
    parsed.open_after_mount = open_value.toBool();
    parsed.from_backend = true;
    *out = parsed;
    return true;
}

} // namespace dwarfsctl_kde
