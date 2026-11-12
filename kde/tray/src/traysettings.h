// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
    const QString base = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString dir = QDir(base).filePath(QStringLiteral("dwarfsctl"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("tray.ini"));
}

inline QSettings traySettings()
{
    return QSettings(traySettingsPath(), QSettings::IniFormat);
}

inline bool advancedUserEnabled()
{
    QSettings s = traySettings();
    return s.value(QStringLiteral("ui/advanced-user"), false).toBool();
}

inline void setAdvancedUserEnabled(bool enabled)
{
    QSettings s = traySettings();
    s.setValue(QStringLiteral("ui/advanced-user"), enabled);
    s.sync();
}

inline int recentlyMountedLimit()
{
    QSettings s = traySettings();
    const int v = s.value(QStringLiteral("recent/limit"), 5).toInt();
    if (v < 0)
        return 0;
    if (v > 9)
        return 9;
    return v;
}

inline void setRecentlyMountedLimit(int limit)
{
    if (limit < 0)
        limit = 0;
    if (limit > 9)
        limit = 9;

    QSettings s = traySettings();
    s.setValue(QStringLiteral("recent/limit"), limit);

    // Enforce the new limit immediately.
    if (limit == 0) {
        s.remove(QStringLiteral("recent/images"));
    } else {
        QStringList imgs = s.value(QStringLiteral("recent/images")).toStringList();
        if (imgs.size() > limit) {
            imgs = imgs.mid(0, limit);
            s.setValue(QStringLiteral("recent/images"), imgs);
        }
    }
    s.sync();
}

inline QStringList recentlyMountedImages()
{
    QSettings s = traySettings();
    return s.value(QStringLiteral("recent/images")).toStringList();
}

inline void clearRecentlyMountedImages()
{
    QSettings s = traySettings();
    s.remove(QStringLiteral("recent/images"));
    s.sync();
}

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
    const int limit = recentlyMountedLimit();
    if (limit <= 0)
        return;

    QSettings s = traySettings();
    const QString canon = canonicalOrOriginal(imagePath);

    QStringList imgs = s.value(QStringLiteral("recent/images")).toStringList();
    imgs.removeAll(canon);
    imgs.prepend(canon);
    if (imgs.size() > limit) {
        imgs = imgs.mid(0, limit);
    }
    s.setValue(QStringLiteral("recent/images"), imgs);
    s.sync();
}

} // namespace dwarfsctl_tray
