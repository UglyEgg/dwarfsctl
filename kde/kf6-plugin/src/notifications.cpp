// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KLocalizedString>

#include "notifications.h"

#include "dwarfsctljson.h"

#include <KNotification>

#include <QDesktopServices>
#include <QUrl>

namespace dwarfsplugin {

NotifyKind classifyEnvelope(const Envelope &env)
{
    // Strict: failures are handled via critical modal; notifications are additive.
    if (!env.ok) {
        return NotifyKind::Error;
    }
    if (!env.warning_objects.isEmpty()) {
        return NotifyKind::Warning;
    }
    return NotifyKind::Info;
}

static QString eventIdForKind(NotifyKind kind)
{
    switch (kind) {
    case NotifyKind::Info:
        return QStringLiteral("dwarfsctl-info");
    case NotifyKind::Warning:
        return QStringLiteral("dwarfsctl-warning");
    case NotifyKind::Error:
        return QStringLiteral("dwarfsctl-error");
    }
    return QStringLiteral("dwarfsctl-info");
}

void notifyResult(QObject *parent, NotifyKind kind, const QString &title, const QString &message, const QString &mountpoint)
{
    auto *n = new KNotification(eventIdForKind(kind), KNotification::CloseOnTimeout, parent);
    n->setTitle(title);
    if (!mountpoint.isEmpty()) {
        n->setText(QStringLiteral("%1\n%2: %3").arg(message, i18n("Mountpoint"), mountpoint));
    } else {
        n->setText(message);
    }

    n->sendEvent();
}

} // namespace dwarfsplugin
