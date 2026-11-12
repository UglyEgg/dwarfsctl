// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KPropertiesDialogPlugin>

namespace dwarfsplugin {

class DwarfsPropertiesPlugin : public KPropertiesDialogPlugin
{
    Q_OBJECT
public:
    explicit DwarfsPropertiesPlugin(QObject *parent, const QVariantList &args);
};

} // namespace dwarfsplugin
