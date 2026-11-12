// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KJob>

#include <QPointer>
#include <QString>
#include <QVariant>

#include "dwarfsctljson.h"

class QDBusPendingCallWatcher;

namespace dwarfsplugin {

// KJob wrapper around the tray's D-Bus mount-agent interface.
//
// This keeps the Dolphin plugin responsive while mount/unmount work happens
// in the tray (which owns Places cleanup and mount state tracking).
class TrayAgentJob final : public KJob
{
    Q_OBJECT

public:
    struct Request {
        QString title;
        QString method;
        QList<QVariant> args;
        int timeout_ms = 4000;
    };

    explicit TrayAgentJob(Request req, QObject* parent = nullptr);
    ~TrayAgentJob() override;

    void start() override;

    Envelope envelope() const { return m_env; }

protected:
    bool doKill() override;

private:
    void finishWithError(const QString& msg, const QString& kind, const QString& name = QString());
    void finishFromJson(const QString& json);

    Request m_req;
    Envelope m_env;
    bool m_killed = false;
    QPointer<QDBusPendingCallWatcher> m_watcher;
};

} // namespace dwarfsplugin
