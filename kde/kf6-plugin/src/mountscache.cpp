// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mountscache.h"

#include "traydbus.h"
#include "logging.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusMessage>
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
        // A later consumer can request recovery, but repeated UI reads cannot
        // activate the service or continually restart a failing snapshot call.
        if (!m_known && !m_seedPending && !m_retryTimer.isActive()
            && !m_owner.isEmpty() && m_lastAttempt.elapsed() >= 5000) {
            m_seedAttempts = 0;
            seedFromAgent();
        }
        return;
    }
    m_started = true;
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &MountsCache::seedFromAgent);
    auto bus = QDBusConnection::sessionBus();
    auto *owners = new QDBusServiceWatcher(QString::fromLatin1(kTrayDbusService), bus,
        QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(owners, &QDBusServiceWatcher::serviceOwnerChanged, this,
        [this](const QString&, const QString&, const QString& owner) { setOwner(owner); });

    // Neither activation nor owner discovery may block a menu or Properties
    // page. The watcher is already installed, so a later owner will reseed even
    // when this best-effort activation fails or takes longer than the call.
    const auto generation = m_generation;
    auto lookup = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
    lookup << QString::fromLatin1(kTrayDbusService);
    auto *ownerReply = new QDBusPendingCallWatcher(bus.asyncCall(lookup, 3000), this);
    connect(ownerReply, &QDBusPendingCallWatcher::finished, this, [this, ownerReply, generation] {
        const QDBusPendingReply<QString> reply = *ownerReply;
        ownerReply->deleteLater();
        if (generation == m_generation && !reply.isError()) setOwner(reply.value());
    });
    auto activation = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.DBus"),
        QStringLiteral("/org/freedesktop/DBus"), QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
    activation << QString::fromLatin1(kTrayDbusService) << quint32(0);
    (void)bus.asyncCall(activation, 3000);
}

void MountsCache::invalidate()
{
    if (m_known) {
        m_known = false;
        Q_EMIT mountsChanged();
    }
}

void MountsCache::setOwner(const QString& owner)
{
    if (owner == m_owner) return;
    auto bus = QDBusConnection::sessionBus();
    if (!m_owner.isEmpty()) {
        bus.disconnect(m_owner, QString::fromLatin1(kMountsDbusPath),
            QString::fromLatin1(kMountsDbusInterface), QStringLiteral("MountsChanged"),
            this, SLOT(onMountsChanged(QString)));
    }
    ++m_generation;
    ++m_revision;
    m_owner = owner;
    m_seedPending = false;
    m_seedAttempts = 0;
    m_retryTimer.stop();
    invalidate();
    if (owner.isEmpty()) return;
    if (!bus.connect(owner, QString::fromLatin1(kMountsDbusPath),
            QString::fromLatin1(kMountsDbusInterface), QStringLiteral("MountsChanged"),
            this, SLOT(onMountsChanged(QString)))) {
        qCWarning(DWARFS_DOLPHIN_LOG) << "Failed to subscribe to MountsChanged";
    }
    seedFromAgent();
}

void MountsCache::seedFailed()
{
    invalidate();
    // Three total attempts per recovery request, with no timer after exhaustion.
    if (m_seedAttempts < 3 && !m_owner.isEmpty()) {
        m_retryTimer.start(m_seedAttempts == 1 ? 100 : 500);
    }
}

void MountsCache::seedFromAgent()
{
    if (m_owner.isEmpty() || m_seedPending) return;
    m_seedPending = true;
    ++m_seedAttempts;
    m_lastAttempt.start();
    const auto generation = m_generation;
    const auto revision = m_revision;
    auto call = QDBusMessage::createMethodCall(m_owner,
        QString::fromLatin1(kMountsDbusPath), QString::fromLatin1(kMountsDbusInterface),
        QStringLiteral("ListMounts"));
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusConnection::sessionBus().asyncCall(call, 3000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
        [this, watcher, generation, revision]() {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (generation != m_generation) return;
        m_seedPending = false;
        // A newer authoritative signal supersedes this snapshot, even when the
        // older snapshot failed or arrives after the signal.
        if (revision != m_revision) {
            if (!m_known) seedFailed();
            return;
        }
        if (reply.isError()) {
            seedFailed();
            return;
        }
        QJsonParseError error {};
        const auto doc = QJsonDocument::fromJson(reply.value().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()
            || doc.object().value(QStringLiteral("ok")) != QJsonValue(true)) {
            seedFailed();
            return;
        }
        const auto root = doc.object();
        auto mounts = root.value(QStringLiteral("details")).toObject().value(QStringLiteral("mounts"));
        if (!mounts.isArray()) mounts = root.value(QStringLiteral("mounts"));
        if (!mounts.isArray() || !applyMountsJsonArray(QString::fromUtf8(
                QJsonDocument(mounts.toArray()).toJson(QJsonDocument::Compact)))) {
            seedFailed();
        }
    });
}

bool MountsCache::hasImage(const QString& imagePath) const
{
    return m_known && m_imageToMountpoint.contains(imagePath);
}

QString MountsCache::mountpointForImage(const QString& imagePath) const
{
    return m_known ? m_imageToMountpoint.value(imagePath) : QString();
}

QString MountsCache::imageForMountpoint(const QString& mountpoint) const
{
    if (!m_known) return {};
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
    return m_known ? m_imageToMountpoint : QHash<QString, QString>();
}

void MountsCache::onMountsChanged(const QString& mountsJson)
{
    if (!calledFromDBus() || message().service() != m_owner || m_owner.isEmpty()) return;
    ++m_revision;
    if (!applyMountsJsonArray(mountsJson)) {
        seedFailed();
    }
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
        if (!v.isObject()) return false;
        const QJsonObject o = v.toObject();
        const QString img = o.value(QStringLiteral("image")).toString();
        const QString mp = o.value(QStringLiteral("mountpoint")).toString();
        if (img.isEmpty() || mp.isEmpty() || next.contains(img)) return false;
        next.insert(img, mp);
    }

    const bool changed = !m_known || m_imageToMountpoint != next;
    m_imageToMountpoint = std::move(next);
    m_known = true;
    m_retryTimer.stop();
    m_seedAttempts = 0;
    if (changed) Q_EMIT mountsChanged();
    return true;
}

} // namespace dwarfsplugin
