// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QJsonObject>
#include <QStringList>
namespace dwarfsplugin {
struct Recommendation {
    bool valid = false;
    QStringList flags;
    QStringList reasons;
    QStringList warnings;
    QString confidence;
};
Recommendation parseRecommendation(const QJsonObject &details);
}
