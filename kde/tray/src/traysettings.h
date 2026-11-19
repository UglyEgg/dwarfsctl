// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "imagehistory.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

namespace dwarfsctl_tray {

// Keep tray state (like recently mounted items) in a user-friendly config file.
inline QString traySettingsPath()
{
    return dwarfsctl_kde::imageHistoryPath();
}

inline QSettings traySettings()
{
    return QSettings(traySettingsPath(), QSettings::IniFormat);
}

inline bool advancedUserEnabled()
{
    dwarfsctl_kde::image_history_detail::FreshSettings fresh(traySettingsPath());
    return fresh.get() && fresh.get()->value(QStringLiteral("ui/advanced-user"), false).toBool();
}

inline bool setAdvancedUserEnabled(bool enabled)
{
    return dwarfsctl_kde::image_history_detail::edit([enabled](dwarfsctl_kde::image_history_detail::SettingsValues &settings) {
        settings.setValue(QStringLiteral("ui/advanced-user"), enabled);
    });
}

inline int recentlyMountedLimit() { return dwarfsctl_kde::recentImageLimit(); }
inline void setRecentlyMountedLimit(int limit) { (void)dwarfsctl_kde::setRecentImageLimit(limit); }
inline QStringList recentlyMountedImages() { return dwarfsctl_kde::recentImages(); }
inline bool clearRecentlyMountedImages() { return dwarfsctl_kde::clearRecentImages(); }

inline QString canonicalOrOriginal(const QString& path)
{
    const QFileInfo fi(path);
    const QString canon = fi.exists() ? fi.canonicalFilePath() : QString();
    return canon.isEmpty() ? path : canon;
}

inline QString shortHashForImage(const QString& imagePath)
{
    const QString canon = canonicalOrOriginal(imagePath);
    const QByteArray bytes = canon.toUtf8();
    const QByteArray hex = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(hex.left(8));
}

inline QString recentLabelForImage(const QString& imagePath)
{
    const QFileInfo fi(imagePath);
    QString stem = fi.completeBaseName();
    if (stem.isEmpty()) {
        stem = fi.fileName();
    }
    if (stem.isEmpty()) {
        stem = QStringLiteral("dwarfs");
    }
    return stem;
}

inline void recordRecentlyMountedImage(const QString& imagePath)
{
    (void)dwarfsctl_kde::rememberImage(imagePath);
}

} // namespace dwarfsctl_tray
