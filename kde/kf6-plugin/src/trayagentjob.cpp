// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "trayagentjob.h"

#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusError>
#include <QDBusConnection>
#include <QTimer>
#include <QJsonObject>

#include "traydbus.h"

using namespace dwarfsplugin;

TrayAgentJob::TrayAgentJob(Request req, QObject* parent)
    : KJob(parent)
    , m_req(std::move(req))
{
    setCapabilities(KJob::Killable);
}

TrayAgentJob::~TrayAgentJob()
{
}

bool TrayAgentJob::doKill()
{
    m_killed = true;
    if (m_watcher) {
        // QDBusPendingCall has no true cancel; ignore result when it arrives.
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    setError(KJob::KilledJobError);
    setErrorText(QStringLiteral("Cancelled"));
    emitResult();
    return true;
}

void TrayAgentJob::start()
{
    using dwarfsplugin::ensureTrayRunning;

    ensureTrayRunning();

    auto bus = QDBusConnection::sessionBus();
    QDBusInterface iface(QString::fromLatin1(dwarfsplugin::kTrayDbusService),
                         QString::fromLatin1(dwarfsplugin::kMountsDbusPath),
                         QString::fromLatin1(dwarfsplugin::kMountsDbusInterface),
                         bus);

    if (!iface.isValid()) {
        finishWithError(QStringLiteral("Tray D-Bus interface not available"), QStringLiteral("dbus"));
        return;
    }

    const QDBusPendingCall call = iface.asyncCallWithArgumentList(m_req.method, m_req.args);
    auto* watcher = new QDBusPendingCallWatcher(call, this);
    m_watcher = watcher;

    // Timeout: treat as failure (avoid hanging Dolphin UI indefinitely).
    if (m_req.timeout_ms > 0) {
        QTimer::singleShot(m_req.timeout_ms, this, [this]() {
            if (m_killed || !m_watcher) {
                return;
            }
            finishWithError(QStringLiteral("Operation timed out"), QStringLiteral("timeout"));
        });
    }

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        if (m_killed) {
            if (w) {
                w->deleteLater();
            }
            return;
        }

        QDBusPendingReply<QString> reply = *w;
        w->deleteLater();
        m_watcher = nullptr;

        if (reply.isError()) {
            const QDBusError e = reply.error();
            finishWithError(e.message().isEmpty() ? QStringLiteral("D-Bus call failed") : e.message(),
                            QStringLiteral("dbus"),
                            e.name());
            return;
        }

        finishFromJson(reply.value());
    });
}

void TrayAgentJob::finishWithError(const QString& msg, const QString& kind, const QString& name)
{
    m_env.ok = false;
    m_env.command = QStringLiteral("dbus:%1").arg(m_req.method);
    m_env.has_error = true;
    m_env.error.code = 1;
    m_env.error.kind = kind;
    m_env.error.message = msg;

    if (!name.isEmpty()) {
        m_env.details.insert(QStringLiteral("dbus_error_name"), name);
    }

    setError(1);
    setErrorText(msg);
    emitResult();
}

void TrayAgentJob::finishFromJson(const QString& json)
{
    const QByteArray bytes = json.toUtf8();
    const auto pr = dwarfsplugin::parseEnvelope(bytes);
    if (!pr.parsed) {
        finishWithError(QStringLiteral("failed to parse tray JSON envelope: %1").arg(pr.error_message),
                        QStringLiteral("parse"));
        return;
    }
    m_env = pr.envelope;
    if (!m_env.ok) {
        const QString msg = !m_env.error.message.isEmpty() ? m_env.error.message
                                                          : QStringLiteral("Operation failed");
        setError(1);
        setErrorText(msg);
        emitResult();
        return;
    }
    setError(0);
    emitResult();
}
