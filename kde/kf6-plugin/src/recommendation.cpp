// SPDX-License-Identifier: GPL-3.0-or-later
#include "recommendation.h"
#include <QJsonArray>
namespace dwarfsplugin {
Recommendation parseRecommendation(const QJsonObject &details)
{
    Recommendation result;
    if (details.value(QStringLiteral("schema")).toString() != QLatin1String("dwarfsctl.recommend.v1"))
        return result;
    const auto rec = details.value(QStringLiteral("recommendation")).toObject();
    auto strings = [&](const QString &key, QStringList &out) {
        const auto value = rec.value(key);
        if (!value.isArray()) return false;
        for (const auto &item : value.toArray()) {
            if (!item.isString() || item.toString().isEmpty()) return false;
            out.append(item.toString());
        }
        return true;
    };
    if (!strings(QStringLiteral("flags_only"), result.flags)
        || !strings(QStringLiteral("reasons"), result.reasons)
        || !strings(QStringLiteral("warnings"), result.warnings)
        || result.flags.isEmpty() || result.reasons.isEmpty()) return result;
    if (rec.contains(QStringLiteral("notes")) && !strings(QStringLiteral("notes"), result.notes)) return result;
    // Older backends sent this routine disclosure as a warning.
    for (auto it = result.warnings.begin(); it != result.warnings.end();) {
        if (it->startsWith(QLatin1String("Metadata heuristic only:"))) {
            result.notes.append(*it);
            it = result.warnings.erase(it);
        } else {
            ++it;
        }
    }
    result.confidence = rec.value(QStringLiteral("confidence")).toString();
    result.valid = result.confidence == QLatin1String("low") || result.confidence == QLatin1String("medium");
    return result;
}
}
