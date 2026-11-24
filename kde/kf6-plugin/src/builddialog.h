// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace dwarfsplugin {

struct BuildDialogResult {
    bool accepted = false;
    QString outputPath;
    QString profileName;
    QStringList profileArgs; // resolved args for selected profile

    bool force = false;
    bool preview = false;
};

// Prompt for build options when the user selects "Build" on a directory.
BuildDialogResult promptBuildOptions(QWidget *parent, const QString &sourceDir);

} // namespace dwarfsplugin
