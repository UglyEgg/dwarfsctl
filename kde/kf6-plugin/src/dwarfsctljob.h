// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <KJob>

#include <QProcess>
#include <QTimer>
#include <QString>
#include <QStringList>
#include <utility>

#include "dwarfsctljson.h"
#include "dwarfsctlrunner.h"

namespace dwarfsplugin {

class DwarfsctlJob final : public KJob {
    Q_OBJECT
public:
    struct Request {
        QString title;
        QStringList args;
        int timeout_ms = 0;
        // Background refreshes (for example, a manager selection changing)
        // should not create a progress dialog for a quick status query.
        bool show_ui = true;
    };

    explicit DwarfsctlJob(Request req, QObject *parent = nullptr);
    ~DwarfsctlJob() override;

    void start() override;
    bool doKill() override;
    QString errorString() const override;

    Envelope envelope() const;

Q_SIGNALS:
    void buildPhase(const QString &phase);

private:
    bool stopOwnedProcess();
    bool m_stopping = false;
    qint64 m_processGroup = 0;
    Request m_req;
    QString m_errorString;
    Envelope m_envelope;
    bool m_hasEnvelope = false;
    QProcess m_proc;
    QTimer m_timeout;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QByteArray m_progressBuffer;
};

} // namespace dwarfsplugin
