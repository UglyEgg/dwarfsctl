// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace dwarfsplugin {

/// The typed error object from a dwarfsctl envelope (`error` key).
///
/// Mirrors `ErrorObject` in cpp/cli/core.cpp, which carries exactly
/// code, kind and message. The image is reported by the envelope's own `image`
/// field, not by the error object.
struct ErrorInfo {
    int code = 1;
    QString kind;
    QString message;
};

struct WarningInfo {
    QString code;
    QString message;
};

struct Envelope {
    bool ok = false;
    QString command;
    QString image;
    QJsonObject details;
    QStringList warnings;
    QVector<WarningInfo> warning_objects;
    bool has_error = false;
    ErrorInfo error;
};

struct ParseResult {
    bool parsed = false;
    Envelope envelope;
    QString error_message;
};

ParseResult parseEnvelope(const QByteArray &json_bytes);

} // namespace dwarfsplugin
