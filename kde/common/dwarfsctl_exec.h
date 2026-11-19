// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace dwarfsctl_kde {

// Explicit overrides select their program; default discovery prefers a sibling
// backend so staged applications use their matching installation before PATH.
inline QString dwarfsctlExe()
{
    const QString override = qEnvironmentVariable("DWARFSCTL_BIN").trimmed();
    const QString program = override.isEmpty() ? QStringLiteral("dwarfsctl") : override;
    if (QDir::isAbsolutePath(program)) return program;
    if (override.isEmpty()) {
        const QString sibling = QStandardPaths::findExecutable(program, {QCoreApplication::applicationDirPath()});
        if (!sibling.isEmpty()) return sibling;
    }
    const QString found = QStandardPaths::findExecutable(program);
    if (!found.isEmpty()) return found;
    const QString candidates[] = {
        QDir::home().filePath(QStringLiteral(".local/bin/") + program),
        QStringLiteral("/usr/local/bin/") + program,
        QStringLiteral("/usr/bin/") + program,
    };
    for (const auto &path : candidates) {
        const QFileInfo info(path);
        if (info.isFile() && info.isExecutable()) return path;
    }
    return program;
}

} // namespace dwarfsctl_kde
