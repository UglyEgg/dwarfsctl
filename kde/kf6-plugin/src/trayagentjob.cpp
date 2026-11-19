// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "trayagentjob.h"

#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusError>
#include <QDBusConnection>
#include <QTimer>
#include <QJsonObject>
#include <KLocalizedString>

#include "traydbus.h"

using namespace dwarfsplugin;

static QString unknownOutcomeMessage()
{
    return i18n("The mount service did not reply in time. The operation may still complete. Check the current mount state before trying again.");
}

TrayAgentJob::TrayAgentJob(Request req, QObject* parent)
    : KJob(parent)
    , m_req(std::move(req))
{
    // The server has no cancellation API. Stopping observation cannot cancel
    // an already dispatched mount operation, so never advertise cancellation.
}

TrayAgentJob::~TrayAgentJob()
{
}

void TrayAgentJob::finish()
{
    if (m_finished) return;
    m_finished = true;
    if (m_watcher) {
        m_watcher->disconnect(this);
        m_watcher->deleteLater();
        m_watcher = nullptr;
    }
    emitResult();
}

void TrayAgentJob::start()
{
    if (m_finished) return;
    auto message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kTrayDbusService), QString::fromLatin1(kMountsDbusPath),
        QString::fromLatin1(kMountsDbusInterface), m_req.method);
    message.setArguments(m_req.args);
    // The bus activates the service while this request remains asynchronous.
    const auto call = QDBusConnection::sessionBus().asyncCall(message, m_req.timeout_ms > 0 ? m_req.timeout_ms : -1);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    m_watcher = watcher;

    // Timeout: treat as failure (avoid hanging Dolphin UI indefinitely).
    if (m_req.timeout_ms > 0) {
        QTimer::singleShot(m_req.timeout_ms, this, [this]() {
            if (m_finished || !m_watcher) {
                return;
            }
            finishWithError(unknownOutcomeMessage(), QStringLiteral("timeout"));
        });
    }

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        if (m_finished) {
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
            if (e.type() == QDBusError::NoReply || e.type() == QDBusError::Timeout || e.type() == QDBusError::TimedOut) {
                finishWithError(unknownOutcomeMessage(), QStringLiteral("timeout"), e.name());
                return;
            }
            finishWithError(e.message().isEmpty() ? i18n("The mount service could not complete the request.") : e.message(),
                            QStringLiteral("dbus"),
                            e.name());
            return;
        }

        finishFromJson(reply.value());
    });
}

void TrayAgentJob::finishWithError(const QString& msg, const QString& kind, const QString& name)
{
    if (m_finished) return;
    m_env.ok = false;
    m_env.command = QStringLiteral("dbus:%1").arg(m_req.method);
    m_env.has_error = true;
    m_env.error.code = 1;
    m_env.error.kind = kind;
    m_env.error.message = msg;

    if (!name.isEmpty()) {
        m_env.details.insert(QStringLiteral("dbus_error_name"), name);
    }

    setError(KJob::UserDefinedError);
    setErrorText(msg);
    finish();
}

void TrayAgentJob::finishFromJson(const QString& json)
{
    if (m_finished) return;
    const QByteArray bytes = json.toUtf8();
    const auto pr = dwarfsplugin::parseEnvelope(bytes);
    if (!pr.parsed) {
        finishWithError(i18n("The mount service returned an invalid response: %1", pr.error_message),
                        QStringLiteral("parse"));
        return;
    }
    m_env = pr.envelope;
    if (!m_env.ok) {
        const QString msg = !m_env.error.message.isEmpty() ? m_env.error.message
                                                          : i18n("The operation failed.");
        setError(KJob::UserDefinedError);
        setErrorText(msg);
        finish();
        return;
    }
    setError(0);
    finish();
}
