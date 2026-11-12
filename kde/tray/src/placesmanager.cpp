// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "placesmanager.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUrl>
#include <QVector>

#include <KFilePlacesModel>

namespace dwarfsctl_tray {

namespace {

QString labelForImage(const QString &imagePath, const QString &mountpoint)
{
    QFileInfo image(imagePath);
    QString label = image.completeBaseName();
    if (label.isEmpty()) {
        label = image.fileName();
    }
    if (label.isEmpty()) {
        label = QFileInfo(mountpoint).fileName();
    }
    if (label.isEmpty()) {
        label = QStringLiteral("DwarFS");
    }
    return label;
}

QString iconForImage(const QString &imagePath)
{
    const QString fallback = QStringLiteral("dwarfsctl-volume");
    QFile sidecar(imagePath + QStringLiteral(".meta.json"));
    if (!sidecar.open(QIODevice::ReadOnly)) {
        return fallback;
    }

    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson(sidecar.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()
        || document.object().value(QStringLiteral("version")).toInt() != 1) {
        return fallback;
    }

    const QString icon = document.object()
                             .value(QStringLiteral("places"))
                             .toObject()
                             .value(QStringLiteral("icon"))
                             .toString();
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9._-]{1,64}$"));
    if (!valid.match(icon).hasMatch()
        || icon == QLatin1String("folder-linux")
        || icon == QLatin1String("drive-removable-media")) {
        return fallback;
    }
    return icon;
}

} // namespace

void syncPlacesEntry(bool add, const QString &imagePath, const QString &mountpoint)
{
    if (mountpoint.isEmpty()) {
        return;
    }

    const QUrl url = QUrl::fromLocalFile(mountpoint);
    if (!url.isValid()) {
        return;
    }

    KFilePlacesModel model;
    QVector<QModelIndex> matches;
    const int rows = model.rowCount();
    for (int i = 0; i < rows; i++) {
        const QModelIndex idx = model.index(i, 0);
        if (!idx.isValid()) {
            continue;
        }
        if (model.url(idx) == url) {
            matches.append(idx);
        }
    }

    for (auto it = matches.crbegin(); it != matches.crend(); ++it) {
        model.removePlace(*it);
    }

    if (add) {
        model.addPlace(labelForImage(imagePath, mountpoint),
                       url,
                       iconForImage(imagePath),
                       QString());
    }
}

void removePlacesEntryForMountpoint(const QString &mountpoint)
{
    syncPlacesEntry(false, QString(), mountpoint);
}

} // namespace dwarfsctl_tray
