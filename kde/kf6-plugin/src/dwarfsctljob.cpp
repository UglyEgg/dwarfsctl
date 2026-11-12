// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctljob.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

#include <KDialogJobUiDelegate>
#include <KLocalizedString>

#include "logging.h"

namespace dwarfsplugin {

static QString bytesToQStringCapped(const QByteArray &b, int maxBytes, bool *outTruncated)
{
    if (b.size() <= maxBytes) {
        if (outTruncated) {
            *outTruncated = false;
        }
        return QString::fromUtf8(b);
    }
    if (outTruncated) {
        *outTruncated = true;
    }
    return QString::fromUtf8(b.left(maxBytes));
}

static void attachStdIoToEnvelope(Envelope *env, const QByteArray &stdoutBytes, const QByteArray &stderrBytes)
{
    if (!env) {
        return;
    }
    // Ensure “Advanced” views can show raw tool output, even when the structured
    // envelope is empty or parsing fails.
    constexpr int kMax = 256 * 1024; // cap to keep dialogs responsive

    bool outTrunc = false;
    const QString so = bytesToQStringCapped(stdoutBytes, kMax, &outTrunc);
    if (!so.isEmpty()) {
        env->details.insert(QStringLiteral("_stdout"), so);
        if (outTrunc) {
            env->details.insert(QStringLiteral("_stdout_truncated"), true);
        }
    }

    outTrunc = false;
    const QString se = bytesToQStringCapped(stderrBytes, kMax, &outTrunc);
    if (!se.isEmpty()) {
        env->details.insert(QStringLiteral("_stderr"), se);
        if (outTrunc) {
            env->details.insert(QStringLiteral("_stderr_truncated"), true);
        }
    }
}

DwarfsctlJob::DwarfsctlJob(Request req, QObject *parent)
    : KJob(parent)
    , m_req(std::move(req))
{
    setAutoDelete(true);
    // Use a real KJob UI delegate so Dolphin presents a standard progress UI
    // (with cancellation) for long-running operations like builds.
    // KF6 API expects both flags and an optional window pointer.
    // We pass nullptr here; the job UI will still be shown and parented sensibly by KDE.
    if (m_req.show_ui) {
        setUiDelegate(new KDialogJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr));
    }

    m_timeout.setSingleShot(true);

    connect(&m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        m_stdout += m_proc.readAllStandardOutput();
    });
    connect(&m_proc, &QProcess::readyReadStandardError, this, [this]() {
        const auto chunk = m_proc.readAllStandardError();
        m_stderr += chunk;
        m_progressBuffer += chunk;
        int newline;
        while ((newline = m_progressBuffer.indexOf('\n')) >= 0) {
            const auto obj = QJsonDocument::fromJson(m_progressBuffer.left(newline)).object();
            m_progressBuffer.remove(0, newline + 1);
            if (obj.value(QStringLiteral("event")).toString() == QLatin1String("build_progress"))
                Q_EMIT buildPhase(obj.value(QStringLiteral("phase")).toString());
        }
        if (m_progressBuffer.size() > 65536) m_progressBuffer.clear();
    });
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        // Most common failure in practice: Dolphin launched from a desktop entry
        // does not inherit the same PATH as an interactive shell, so dwarfsctl
        // (often installed in ~/.local/bin) cannot be found/executed.
        setError(1);
        m_errorString = m_proc.errorString();
        m_envelope.ok = false;
        m_envelope.has_error = true;
        m_envelope.error.kind = QStringLiteral("spawn");
        m_envelope.error.message = i18n("Failed to start '%1': %2", m_proc.program(), m_proc.errorString());
        attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
        m_hasEnvelope = true;

        setErrorText(m_envelope.error.message);
        qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl process error" << e << m_proc.program() << m_proc.errorString();
        emitResult();
    });
    connect(&m_proc, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitStatus)
        m_timeout.stop();

        // Drain any remaining buffered output.
        m_stdout += m_proc.readAllStandardOutput();
        m_stderr += m_proc.readAllStandardError();

        if (error() != 0) {
            // errorOccurred already set an error and emitted result.
            return;
        }

        // Even when dwarfsctl exits non-zero, it still emits a JSON envelope in mode.
        // Parse stdout first so we can surface structured errors and warning_objects to the UI.
        const auto parsedMaybe = parseEnvelope(m_stdout);
        if (parsedMaybe.parsed) {
            m_envelope = parsedMaybe.envelope;
            attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
            m_hasEnvelope = true;

            if (!m_envelope.ok) {
                setError(1);
                m_errorString = m_envelope.error.message.isEmpty() ? i18n("Operation failed") : m_envelope.error.message;
                setErrorText(m_envelope.error.message.isEmpty() ? QString::fromUtf8(m_stderr) : m_envelope.error.message);
                qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl returned ok=false" << m_envelope.command << m_envelope.error.message;
                emitResult();
                return;
            }

            if (exitCode != 0) {
                // Unusual: ok=true but non-zero exit. Preserve envelope but report failure.
                setError(1);
                m_errorString = i18n("dwarfsctl failed");
                const QString stderrText = QString::fromUtf8(m_stderr);
                const QString stdoutText = QString::fromUtf8(m_stdout);
                setErrorText(stderrText.isEmpty() ? stdoutText : stderrText);
                qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl exited" << exitCode << stderrText;
                emitResult();
                return;
            }

            Q_EMIT infoMessage(this, i18n("Completed"));
            emitResult();
            return;
        }

        if (exitCode != 0) {
            setError(1);
            m_errorString = i18n("dwarfsctl failed");
            const QString stderrText = QString::fromUtf8(m_stderr);
            const QString stdoutText = QString::fromUtf8(m_stdout);
            setErrorText(stderrText.isEmpty() ? stdoutText : stderrText);
            qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl exited" << exitCode << stderrText;
            // In this branch the JSON envelope could not be parsed, but users still
            // need useful structured information in the dialogs.
            m_envelope.ok = false;
            m_envelope.has_error = true;
            m_envelope.error.kind = QStringLiteral("cli");
            m_envelope.error.message = stderrText.isEmpty() ? stdoutText : stderrText;
            attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
            m_hasEnvelope = true;
            emitResult();
            return;
        }

        const auto parsed = parseEnvelope(m_stdout);
        if (!parsed.parsed) {
            setError(1);
            m_errorString = i18n("Failed to parse dwarfsctl JSON output");
            setErrorText(parsed.error_message.isEmpty() ? QString::fromUtf8(m_stdout) : parsed.error_message);
            qCWarning(DWARFS_DOLPHIN_LOG) << "failed to parse dwarfsctl JSON" << parsed.error_message;
            attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
            emitResult();
            return;
        }

        m_envelope = parsed.envelope;
        attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
        m_hasEnvelope = true;

        if (!m_envelope.ok) {
            setError(1);
            m_errorString = m_envelope.error.message.isEmpty() ? i18n("Operation failed") : m_envelope.error.message;
            setErrorText(m_envelope.error.message);
            qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl returned ok=false" << m_envelope.command << m_envelope.error.message;
            emitResult();
            return;
        }

        Q_EMIT infoMessage(this, i18n("Completed"));
        emitResult();
    });

    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        if (m_proc.state() != QProcess::NotRunning) {
            qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl timed out" << m_req.args;
            m_proc.kill();
            setError(1);
            m_errorString = i18n("Timed out");
            setErrorText(i18n("Operation exceeded configured timeout"));
            // Provide structured error info for the Status dialog.
            m_envelope.ok = false;
            m_envelope.has_error = true;
            m_envelope.error.kind = QStringLiteral("timeout");
            m_envelope.error.message = i18n("dwarfsctl did not finish before the timeout");
            attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
            m_hasEnvelope = true;
            emitResult();
        }
    });
}

DwarfsctlJob::~DwarfsctlJob()
{
    m_timeout.stop();
    if (m_proc.state() != QProcess::NotRunning) {
        // A parent window may close while a background status refresh is still
        // running. Disconnect first so QProcess cannot emit a late job result
        // into an object graph that is being destroyed.
        m_proc.disconnect(this);
        m_proc.kill();
        m_proc.waitForFinished(1000);
    }
}

void DwarfsctlJob::start()
{
    if (m_req.show_ui && !m_req.title.isEmpty()) {
        const QString cmdDisplay = QStringLiteral("%1 %2").arg(DwarfsctlRunner::dwarfsctlProgram(), m_req.args.join(QLatin1Char(' ')));
        const QString timeoutDisplay = m_req.timeout_ms > 0 ? QString::number(m_req.timeout_ms) + QStringLiteral(" ms") : i18n("(none)");
        Q_EMIT description(this,
                         m_req.title,
                         QPair<QString, QString>(i18n("Command"), cmdDisplay),
                         QPair<QString, QString>(i18n("Timeout"), timeoutDisplay));
    }

    Q_EMIT infoMessage(this, i18n("Starting…"));

    const QString program = DwarfsctlRunner::dwarfsctlProgram();
    qCInfo(DWARFS_DOLPHIN_LOG) << "running dwarfsctl" << program << m_req.args;

    m_proc.setProgram(program);
    m_proc.setArguments(m_req.args);
    m_proc.setProcessChannelMode(QProcess::SeparateChannels);
    m_stdout.clear();
    m_stderr.clear();

    m_proc.start();

    if (m_req.timeout_ms > 0) {
        m_timeout.start(m_req.timeout_ms);
    }
}

bool DwarfsctlJob::doKill()
{
    if (m_proc.state() == QProcess::NotRunning) {
        return true;
    }
    qCInfo(DWARFS_DOLPHIN_LOG) << "killing dwarfsctl" << m_req.args;
    m_proc.kill();
    return true;
}

QString DwarfsctlJob::errorString() const
{
    if (!m_errorString.isEmpty()) {
        return m_errorString;
    }
    return KJob::errorString();
}

Envelope DwarfsctlJob::envelope() const
{
    return m_envelope;
}

} // namespace dwarfsplugin
