// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

class QObject;

namespace dwarfsplugin {

struct Envelope;

enum class NotifyKind { Info, Warning, Error };

NotifyKind classifyEnvelope(const Envelope &env);

void notifyResult(QObject *parent,
                  NotifyKind kind,
                  const QString &title,
                  const QString &message,
                  const QString &mountpoint = QString());

} // namespace dwarfsplugin
