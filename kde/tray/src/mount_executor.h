// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Minimal backend abstraction so the mount agent can be refactored without
// coupling D-Bus glue directly to TrayController internals.
class IMountBackend {
public:
    virtual ~IMountBackend() = default;

    // Current mount list as a compact JSON array.
    virtual QString mountsJson() const = 0;
};

class MountExecutor {
public:
    explicit MountExecutor(IMountBackend* backend)
        : m_backend(backend)
    {
    }

    QByteArray listMounts();
    QByteArray status(const QString& imagePath);
    QByteArray mount(const QString& imagePath);
    QByteArray unmountByMountpoint(const QString& mountpoint);
    QByteArray unmountByImage(const QString& imagePath);
    QByteArray unmountAll();
    QByteArray unmountLast();

private:
    IMountBackend* m_backend = nullptr;
};
