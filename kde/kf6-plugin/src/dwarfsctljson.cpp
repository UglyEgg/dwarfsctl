// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctljson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>

namespace dwarfsplugin {

static QString valType(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Bool: return QStringLiteral("bool");
    case QJsonValue::Double: return QStringLiteral("number");
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Object: return QStringLiteral("object");
    case QJsonValue::Undefined: return QStringLiteral("undefined");
    }
    return QStringLiteral("unknown");
}

ParseResult parseEnvelope(const QByteArray &json_bytes)
{
    ParseResult pr;

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json_bytes, &pe);
    if (pe.error != QJsonParseError::NoError) {
        pr.error_message = QStringLiteral("JSON parse error at offset %1: %2")
                               .arg(pe.offset)
                               .arg(pe.errorString());
        return pr;
    }
    if (!doc.isObject()) {
        pr.error_message = QStringLiteral("Expected JSON object envelope, got %1")
                               .arg(doc.isArray() ? QStringLiteral("array") : QStringLiteral("non-object"));
        return pr;
    }

    const QJsonObject root = doc.object();

    const QJsonValue okv = root.value(QStringLiteral("ok"));
    if (!okv.isBool()) {
        pr.error_message = QStringLiteral("Envelope missing boolean 'ok' (got %1)").arg(valType(okv));
        return pr;
    }

    pr.envelope.ok = okv.toBool();
    pr.envelope.command = root.value(QStringLiteral("command")).toString();
    pr.envelope.image = root.value(QStringLiteral("image")).toString();

    const QJsonValue detailsv = root.value(QStringLiteral("details"));
    if (detailsv.isObject()) {
        pr.envelope.details = detailsv.toObject();
    }

    const QJsonValue warningsv = root.value(QStringLiteral("warnings"));
    if (warningsv.isArray()) {
        const auto arr = warningsv.toArray();
        pr.envelope.warnings.reserve(arr.size());
        for (const auto &v : arr) {
            if (v.isString()) {
                pr.envelope.warnings << v.toString();
            }
        }
    }

    const QJsonValue wobjv = root.value(QStringLiteral("warning_objects"));
    if (wobjv.isArray()) {
        const auto arr = wobjv.toArray();
        pr.envelope.warning_objects.reserve(arr.size());
        for (const auto &v : arr) {
            if (!v.isObject()) {
                continue;
            }
            const auto o = v.toObject();
            WarningInfo wi;
            wi.code = o.value(QStringLiteral("code")).toString();
            wi.message = o.value(QStringLiteral("message")).toString();
            if (!wi.code.isEmpty() || !wi.message.isEmpty()) {
                pr.envelope.warning_objects.push_back(wi);
            }
        }
    }

    const QJsonValue errv = root.value(QStringLiteral("error"));
    if (errv.isObject()) {
        pr.envelope.has_error = true;
        const QJsonObject eo = errv.toObject();
        pr.envelope.error.code = eo.value(QStringLiteral("code")).toInt(1);
        pr.envelope.error.kind = eo.value(QStringLiteral("kind")).toString();
        pr.envelope.error.message = eo.value(QStringLiteral("message")).toString();
    }

    pr.parsed = true;
    return pr;
}

} // namespace dwarfsplugin
