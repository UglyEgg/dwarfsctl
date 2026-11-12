// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

#include "dwarfsctljson.h"

namespace dwarfsplugin {

// Unified details viewer for dwarfsctl JSON envelopes.
// This is the single place in the plugin where we render:
// - warning_objects[]
// - error
// - tool payloads (details.tools.* or details.tool)
// - raw JSON
class DetailsDialog : public QDialog {
    Q_OBJECT
public:
    explicit DetailsDialog(const Envelope &env, const QString &title, QWidget *parent = nullptr);
};

// Strict critical modal with a "Show details" affordance.
// Returns true if the user opened the details dialog.
bool criticalModalWithDetails(QWidget *parent, const QString &title, const QString &message, const Envelope &env);

} // namespace dwarfsplugin
