// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "dwarfsctljob.h"
#include "uiconfig.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <functional>

namespace dwarfsplugin {

struct BackendReply {
    bool ok = false;
    Envelope envelope;
    QString text;
    QString error;
};

class BackendQuery {
public:
    // This deadline requests stop. Completion still waits for verified cleanup;
    // a kernel-blocked process does not justify an early success or failure.
    // Destroying the consumer disposes the query without waiting for that stop.
    static constexpr int DeadlineMs = 5000;
    static constexpr qsizetype ResponseLimit = 1024 * 1024;
    using Reply = std::function<void(const BackendReply &)>;
    using UiReply = std::function<void(bool, const dwarfsctl_kde::UiConfig &, const QString &)>;

    static DwarfsctlJob *json(QObject *context, const QStringList &args, Reply complete)
    {
        return start(context, args, DwarfsctlJob::OutputFormat::Json, std::move(complete));
    }

    static DwarfsctlJob *text(QObject *context, const QStringList &args, Reply complete)
    {
        return start(context, args, DwarfsctlJob::OutputFormat::Text, std::move(complete));
    }

    static DwarfsctlJob *ui(QObject *context, UiReply complete)
    {
        return json(context, {QStringLiteral("config"), QStringLiteral("show"),
                              QStringLiteral("--section"), QStringLiteral("ui")},
                    [complete = std::move(complete)](const BackendReply &reply) {
            dwarfsctl_kde::UiConfig config;
            const QJsonObject envelope{{QStringLiteral("ok"), reply.ok},
                                       {QStringLiteral("details"), reply.envelope.details}};
            const bool valid = reply.ok && dwarfsctl_kde::parseUiConfig(QJsonDocument(envelope).toJson(), &config);
            complete(valid, config, valid ? QString() : reply.ok
                ? QObject::tr("The DwarFS settings response is invalid. Retry after checking the backend.")
                : reply.error);
        });
    }

private:
    static DwarfsctlJob *start(QObject *context, const QStringList &args,
                               DwarfsctlJob::OutputFormat format, Reply complete)
    {
        Q_ASSERT(context);
        DwarfsctlJob::Request request;
        request.args = args;
        request.timeout_ms = DeadlineMs;
        request.show_ui = false;
        request.outputFormat = format;
        request.responseLimit = ResponseLimit;
        auto *job = new DwarfsctlJob(request, context);
        job->setUiDelegate(nullptr);
        QObject::connect(job, &KJob::result, context, [job, complete = std::move(complete)] {
            BackendReply reply;
            reply.ok = job->error() == 0;
            reply.envelope = job->envelope();
            reply.text = job->textOutput();
            if (!reply.ok) reply.error = job->errorString();
            complete(reply);
        });
        QTimer::singleShot(0, job, [job] { job->start(); });
        return job;
    }
};

// A surface may retain one current read intent while an earlier owned read
// stops. Busy is known to precede process admission; other errors, especially
// ambiguous mutation outcomes, must never use this retry policy.
class BackendReadRetry final : public QObject {
public:
    explicit BackendReadRetry(QObject *context) : QObject(context)
    {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            auto next = std::move(m_next);
            if (next) next();
        });
    }
    bool schedule(const Envelope &envelope, std::function<void()> next)
    {
        if (envelope.error.kind != QLatin1String("busy")) { cancel(); return false; }
        if (!m_budget.isValid()) m_budget.start();
        if (m_budget.elapsed() >= 5000) { cancel(); return false; }
        m_next = std::move(next);
        m_timer.start(100);
        return true;
    }
    void cancel()
    {
        m_timer.stop();
        m_next = {};
        m_budget.invalidate();
    }
private:
    QTimer m_timer;
    QElapsedTimer m_budget;
    std::function<void()> m_next;
};

} // namespace dwarfsplugin
