// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QHash>
#include <QString>

namespace dwarfsplugin {

// Lightweight client for the tray-owned mount-agent interface.
//
// Goals:
// - Subscribe to MountsChanged so UI can react without polling.
// - Provide a best-effort cache to answer "is this image mounted?" quickly.
// - Seed initial state via ListMounts().
class MountsCache final : public QObject
{
    Q_OBJECT

public:
    static MountsCache* instance();

    // Ensure signal subscription and initial seed have occurred.
    void ensureStarted();

    bool hasImage(const QString& imagePath) const;
    QString mountpointForImage(const QString& imagePath) const;
    QString imageForMountpoint(const QString& mountpoint) const;
    QHash<QString, QString> mounts() const;

// NOTE: The build sets QT_NO_KEYWORDS, so we must use the Qt macro forms
// (Q_SIGNALS/Q_SLOTS) rather than the 'signals'/'slots' keywords.
Q_SIGNALS:
    void mountsChanged();

private Q_SLOTS:
    void onMountsChanged(const QString& mountsJson);

private:
    explicit MountsCache(QObject* parent = nullptr);

    void seedFromAgent();
    bool applyMountsJsonArray(const QString& mountsJson);

    bool m_started = false;
    QHash<QString, QString> m_imageToMountpoint;
};

} // namespace dwarfsplugin
