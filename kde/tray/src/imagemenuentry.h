// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <KLocalizedString>
#include "imagelabel.h"

namespace dwarfsctl_tray {
enum class ImageMenuAction { Mount, OpenMountedFolder, LocateInUtility };
struct ImageMenuEntry {
    QString label;
    QString tooltip;
    ImageMenuAction action;
    QString target;
};
inline QString imageMenuLabel(const QString &image, const QStringList &peers)
{
    const QFileInfo info(image);
    QString name = info.completeBaseName();
    if (name.isEmpty()) name = info.fileName();
    if (name.isEmpty()) name = image;
    const auto hint = dwarfsctl_kde::imageParentHint(image, peers);
    if (!hint.isEmpty()) return i18n("%1 — %2", name, hint);
    return name;
}
// Availability and mount state are distinct: an existing mount stays useful
// after the source image is moved or the device holding it is disconnected.
inline ImageMenuEntry recentImageMenuEntry(const QString &image, const QStringList &peers,
                                           const QString &mountpoint, bool sourceAvailable)
{
    const QString name = imageMenuLabel(image, peers);
    const QString path = QDir::toNativeSeparators(image);
    if (!mountpoint.isEmpty()) {
        return {i18n("%1 — Mounted", name),
            sourceAvailable ? i18n("Open the mounted folder.\n%1", path)
                            : i18n("Open the mounted folder. The source image is unavailable.\n%1", path),
            ImageMenuAction::OpenMountedFolder, mountpoint};
    }
    if (!sourceAvailable) {
        return {i18n("%1 — Unavailable", name), i18n("Open Utility to locate this image.\n%1", path),
            ImageMenuAction::LocateInUtility, image};
    }
    return {name, i18n("Mount this image.\n%1", path), ImageMenuAction::Mount, image};
}
}
