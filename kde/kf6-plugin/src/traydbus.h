// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QString>

namespace dwarfsplugin {

// Keep the D-Bus name aligned with the canonical domain (entropy.quest).
static constexpr const char* kTrayDbusService = "org.quest.entropy.dwarfsctl.Tray";
static constexpr const char* kTrayDbusPath = "/org/quest/entropy/dwarfsctl/Tray";
static constexpr const char* kTrayDbusInterface = "org.quest.entropy.dwarfsctl.Tray";

// Mount-agent interface (tray-owned API).
static constexpr const char* kMountsDbusPath = "/org/quest/entropy/dwarfsctl/Mounts";
static constexpr const char* kMountsDbusInterface = "org.quest.entropy.dwarfsctl.Mounts1";

inline void ensureTrayRunning()
{
    // Best-effort. If the tray isn't installed or D-Bus isn't available, do nothing.
    auto bus = QDBusConnection::sessionBus();
    auto *iface = bus.interface();
    if (!iface) {
        return;
    }

    (void)iface->startService(QString::fromLatin1(kTrayDbusService));

    // Poke the service so activation completes before we return.
    QDBusInterface tray(QString::fromLatin1(kTrayDbusService),
                        QString::fromLatin1(kTrayDbusPath),
                        QString::fromLatin1(kTrayDbusInterface),
                        bus);
    if (tray.isValid()) {
        tray.call(QStringLiteral("Ping"));
    }
}

inline void showTrayAbout()
{
    auto bus = QDBusConnection::sessionBus();
    auto *iface = bus.interface();
    if (!iface) {
        return;
    }

    ensureTrayRunning();

    QDBusInterface tray(QString::fromLatin1(kTrayDbusService),
                        QString::fromLatin1(kTrayDbusPath),
                        QString::fromLatin1(kTrayDbusInterface),
                        bus);
    if (tray.isValid()) {
        tray.call(QStringLiteral("ShowAbout"));
    }
}

} // namespace dwarfsplugin
