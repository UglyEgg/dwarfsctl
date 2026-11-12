// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mountscache.h"

#include "traydbus.h"
#include "logging.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dwarfsplugin {

static MountsCache* g_instance = nullptr;

MountsCache* MountsCache::instance()
{
    if (!g_instance) {
        g_instance = new MountsCache();
    }
    return g_instance;
}

MountsCache::MountsCache(QObject* parent)
    : QObject(parent)
{
}

void MountsCache::ensureStarted()
{
    if (m_started) {
        return;
    }
    m_started = true;

    // Ensure tray is activated (best-effort). If this fails, we still allow the
    // plugin to function in degraded mode.
    ensureTrayRunning();

    auto bus = QDBusConnection::sessionBus();

    const bool ok = bus.connect(QString::fromLatin1(kTrayDbusService),
                                QString::fromLatin1(kMountsDbusPath),
                                QString::fromLatin1(kMountsDbusInterface),
                                QStringLiteral("MountsChanged"),
                                this,
                                SLOT(onMountsChanged(QString)));

    if (!ok) {
        qCWarning(DWARFS_DOLPHIN_LOG) << "Failed to subscribe to MountsChanged";
    }

    seedFromAgent();
}

void MountsCache::seedFromAgent()
{
    auto bus = QDBusConnection::sessionBus();
    QDBusInterface mounts(QString::fromLatin1(kTrayDbusService),
                          QString::fromLatin1(kMountsDbusPath),
                          QString::fromLatin1(kMountsDbusInterface),
                          bus);
    if (!mounts.isValid()) {
        return;
    }

    auto *watcher = new QDBusPendingCallWatcher(mounts.asyncCall(QStringLiteral("ListMounts")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qCWarning(DWARFS_DOLPHIN_LOG) << "Could not seed mount cache" << reply.error().message();
            return;
        }

        // ListMounts returns a dwarfsctl JSON envelope. Extract mounts array.
        QJsonParseError perr {};
        const QJsonDocument doc = QJsonDocument::fromJson(reply.value().toUtf8(), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }

        const QJsonObject root = doc.object();
        if (!root.value(QStringLiteral("ok")).toBool()) {
            return;
        }

        QJsonValue mountsV;
        const QJsonValue detailsV = root.value(QStringLiteral("details"));
        if (detailsV.isObject()) {
            mountsV = detailsV.toObject().value(QStringLiteral("mounts"));
        }
        if (!mountsV.isArray()) {
            mountsV = root.value(QStringLiteral("mounts"));
        }
        if (!mountsV.isArray()) {
            return;
        }

        const QJsonDocument arrDoc(mountsV.toArray());
        if (applyMountsJsonArray(QString::fromUtf8(arrDoc.toJson(QJsonDocument::Compact)))) {
            Q_EMIT mountsChanged();
        }
    });
}

bool MountsCache::hasImage(const QString& imagePath) const
{
    return m_imageToMountpoint.contains(imagePath);
}

QString MountsCache::mountpointForImage(const QString& imagePath) const
{
    return m_imageToMountpoint.value(imagePath);
}

QString MountsCache::imageForMountpoint(const QString& mountpoint) const
{
    const QString wanted = QDir::cleanPath(mountpoint);
    for (auto it = m_imageToMountpoint.cbegin(); it != m_imageToMountpoint.cend(); ++it) {
        if (QDir::cleanPath(it.value()) == wanted) {
            return it.key();
        }
    }
    return {};
}

QHash<QString, QString> MountsCache::mounts() const
{
    return m_imageToMountpoint;
}

void MountsCache::onMountsChanged(const QString& mountsJson)
{
    // Only announce a change if the payload was actually applied. Emitting after
    // a rejected payload would tell every subscriber that mount state changed
    // when nothing did, and a subscriber that reacts by re-querying would do
    // pointless work on every malformed message.
    const bool applied = applyMountsJsonArray(mountsJson);
    if (!applied) {
        qCWarning(DWARFS_DOLPHIN_LOG)
            << "MountsChanged payload could not be applied; not notifying subscribers";
        return;
    }
    // QT_NO_KEYWORDS disables the `emit` keyword; use Q_EMIT instead.
    Q_EMIT mountsChanged();
}

bool MountsCache::applyMountsJsonArray(const QString& mountsJson)
{
    QJsonParseError perr {};
    const QJsonDocument doc = QJsonDocument::fromJson(mountsJson.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }

    QHash<QString, QString> next;
    const QJsonArray a = doc.array();
    for (const auto& v : a) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        const QString img = o.value(QStringLiteral("image")).toString();
        const QString mp = o.value(QStringLiteral("mountpoint")).toString();
        if (!img.isEmpty() && !mp.isEmpty()) {
            next.insert(img, mp);
        }
    }

    m_imageToMountpoint = std::move(next);
    return true;
}

} // namespace dwarfsplugin
