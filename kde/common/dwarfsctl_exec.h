// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace dwarfsctl_kde {

inline QString dwarfsctlExe()
{
    static const QString exe = []() {
        // When launched via D-Bus activation, PATH can be minimal. First, look for a sibling
        // `dwarfsctl` in the same directory as the current executable (common install layout).
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            const QString inAppDir = QStandardPaths::findExecutable(QStringLiteral("dwarfsctl"), {appDir});
            if (!inAppDir.isEmpty()) {
                return inAppDir;
            }
        }

        // Normal PATH-based lookup.
        const QString found = QStandardPaths::findExecutable(QStringLiteral("dwarfsctl"));
        if (!found.isEmpty()) {
            return found;
        }

        // Best-effort fallbacks for common prefixes.
        const QString home = QDir::homePath();
        const QString candidates[] = {
            home.isEmpty() ? QString() : QDir(home).filePath(QStringLiteral(".local/bin/dwarfsctl")),
            QStringLiteral("/usr/local/bin/dwarfsctl"),
            QStringLiteral("/usr/bin/dwarfsctl"),
        };
        for (const auto &c : candidates) {
            if (c.isEmpty()) {
                continue;
            }
            QFileInfo fi(c);
            if (fi.exists() && fi.isFile() && fi.isExecutable()) {
                return c;
            }
        }

        return QStringLiteral("dwarfsctl");
    }();
    return exe;
}

} // namespace dwarfsctl_kde
