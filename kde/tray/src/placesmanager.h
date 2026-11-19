// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace dwarfsctl_tray {

// Keep Dolphin's visible sidebar entry aligned with the tray-owned mount
// registry. This works for every mount path, including CLI and MIME launches.
QString placesIconForImage(const QString &imagePath);
void syncPlacesEntry(bool add, const QString &imagePath, const QString &mountpoint,
                     const QString &icon = QStringLiteral("dwarfsctl-volume"));

// Compatibility helper for cleanup call sites that only know the mountpoint.
void removePlacesEntryForMountpoint(const QString &mountpoint);

} // namespace dwarfsctl_tray
