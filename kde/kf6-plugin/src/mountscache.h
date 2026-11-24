// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <QDBusContext>
#include <QElapsedTimer>
#include <QTimer>

namespace dwarfsplugin {

// Lightweight client for the tray-owned mount-agent interface.
//
// Goals:
// - Subscribe to MountsChanged so UI can react without polling.
// - Provide a best-effort cache to answer "is this image mounted?" quickly.
// - Seed initial state via ListMounts().
class MountsCache final : public QObject, protected QDBusContext
{
    Q_OBJECT

public:
    static MountsCache* instance();

    // Start owner tracking; coalesce snapshot recovery on later demand.
    void ensureStarted();

    // False means unavailable, not a verified empty mount set. Getters expose
    // only current authoritative data; the last map is retained internally.
    bool isKnown() const { return m_known; }
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

    void setOwner(const QString& owner);
    void seedFromAgent();
    void seedFailed();
    void invalidate();
    bool applyMountsJsonArray(const QString& mountsJson);

    bool m_started = false;
    bool m_known = false;
    bool m_seedPending = false;
    QString m_owner;
    quint64 m_generation = 0;
    quint64 m_revision = 0;
    int m_seedAttempts = 0;
    QTimer m_retryTimer;
    QElapsedTimer m_lastAttempt;
    QHash<QString, QString> m_imageToMountpoint;
};

} // namespace dwarfsplugin
