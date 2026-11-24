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

class OwnedJobProcess;

class DwarfsctlJob final : public KJob {
    Q_OBJECT
public:
    enum class OutputFormat { Json, Text };
    struct Request {
        QString title;
        QStringList args;
        int timeout_ms = 0;
        // Background refreshes (for example, a manager selection changing)
        // should not create a progress dialog for a quick status query.
        bool show_ui = true;
        OutputFormat outputFormat = OutputFormat::Json;
        qsizetype responseLimit = 0;
    };

    explicit DwarfsctlJob(Request req, QObject *parent = nullptr);
    ~DwarfsctlJob() override;

    void start() override;
    bool doKill() override;
    QString errorString() const override;

    Envelope envelope() const;
    QString textOutput() const;

Q_SIGNALS:
    void buildPhase(const QString &phase);

private:
    void readOutput(QProcess::ProcessChannel channel);
    bool captureOutput(QByteArray &target, const QByteArray &bytes);
    bool m_responseExceeded = false;
    void initializeProcess(const QString &resource);
    void completeAfterCleanup();
    bool m_started = false;
    void resolveOutput(int exitCode, const ParseResult &parsed);
    void finishWhenStopped();
    bool m_terminalPending = false;
    bool m_resultEmitted = false;
    bool m_stopping = false;
    bool m_cancelRequested = false;
    OwnedJobProcess *m_owned = nullptr;
    Request m_req;
    QString m_errorString;
    Envelope m_envelope;
    bool m_hasEnvelope = false;
    QProcess *m_proc = nullptr;
    QTimer m_timeout;
    QByteArray m_stdout;
    QByteArray m_stderr;
    QByteArray m_progressBuffer;
};

} // namespace dwarfsplugin
