// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QString>

namespace dwarfsplugin {

// Keep the D-Bus name aligned with the canonical domain (entropy.quest).
static constexpr const char* kTrayDbusService = "org.quest.entropy.dwarfsctl.Tray";
static constexpr const char* kTrayDbusPath = "/org/quest/entropy/dwarfsctl/Tray";
static constexpr const char* kTrayDbusInterface = "org.quest.entropy.dwarfsctl.Tray";

// Mount-agent interface (tray-owned API).
static constexpr const char* kMountsDbusPath = "/org/quest/entropy/dwarfsctl/Mounts";
static constexpr const char* kMountsDbusInterface = "org.quest.entropy.dwarfsctl.Mounts1";

inline void showTrayAbout()
{
    const auto message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kTrayDbusService), QString::fromLatin1(kTrayDbusPath),
        QString::fromLatin1(kTrayDbusInterface), QStringLiteral("ShowAbout"));
    QDBusConnection::sessionBus().asyncCall(message);
}

} // namespace dwarfsplugin
