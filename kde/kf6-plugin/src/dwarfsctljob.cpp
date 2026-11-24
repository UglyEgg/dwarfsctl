// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctljob.h"

#include <QDebug>
#include <QCoreApplication>
#include <QAbstractEventDispatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <csignal>
#include <cerrno>
#include <dirent.h>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QHash>
#include <QPointer>
#include <QThread>
#include <QVariant>
#include <functional>
#include <sys/syscall.h>
#include <unistd.h>

#include <KDialogJobUiDelegate>
#include <KLocalizedString>

#include "logging.h"

namespace dwarfsplugin {
namespace {
// Linux UAPI PIDFD_SIGNAL_PROCESS_GROUP (available since Linux 6.9).
constexpr unsigned kSignalProcessGroup = 1U << 2;
enum class GroupState { Stopped, Active, Unknown };
class GroupObservations {
public:
    QHash<qint64, GroupState> groups;
    GroupState state(qint64 group, int descriptor)
    {
        if (group <= 0) return GroupState::Stopped;
        if (descriptor >= 0
            && ::syscall(SYS_pidfd_send_signal, descriptor, 0, nullptr, kSignalProcessGroup) < 0
            && errno == ESRCH) return GroupState::Stopped;
        if (!observed) scan();
        return groups.value(group, GroupState::Unknown);
    }
private:
    void scan()
    {
        observed = true;
        DIR *directory = ::opendir("/proc");
        if (!directory) {
            for (auto &state : groups) state = GroupState::Unknown;
            return;
        }
        errno = 0;
        while (auto *entry = ::readdir(directory)) {
            bool numeric = false;
            const auto pid = QByteArray(entry->d_name).toLongLong(&numeric);
            if (!numeric || pid <= 0) { errno = 0; continue; }
            const auto group = ::getpgid(pid);
            auto state = groups.find(group);
            if (state == groups.end() || ::getsid(pid) != group) { errno = 0; continue; }
            QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
            if (!file.open(QIODevice::ReadOnly)) {
                if (::getpgid(pid) == group && ::getsid(pid) == group && *state != GroupState::Active)
                    *state = GroupState::Unknown;
                errno = 0;
                continue;
            }
            const auto raw = file.readAll();
            const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
            if (fields.size() < 20) {
                if (*state != GroupState::Active) *state = GroupState::Unknown;
            } else if (fields.at(0) != "Z" && fields.at(0) != "X") *state = GroupState::Active;
            errno = 0;
        }
        if (errno != 0) for (auto &state : groups) if (state == GroupState::Stopped) state = GroupState::Unknown;
        ::closedir(directory);
    }
    bool observed = false;
};

QString readResource(const QStringList &args)
{
    // Global CLI options can precede the command. Normalize paths lexically:
    // resolving a symlink here could itself block on the unavailable image.
    int command = 0;
    while (command < args.size() && args.at(command).startsWith(QLatin1String("--"))) {
        if (args.at(command) == QLatin1String("--config")) command += 2;
        else if (args.at(command).startsWith(QLatin1String("--config=")) || args.at(command) == QLatin1String("--verbose")) ++command;
        else return {};
    }
    if (command + 1 >= args.size()) return {};
    const auto op = args.at(command);
    if (op != QLatin1String("status") && op != QLatin1String("usage") && op != QLatin1String("ls")
        && op != QLatin1String("cat") && op != QLatin1String("peek")) return {};
    return op + QLatin1Char('\n') + QDir::cleanPath(QDir::current().absoluteFilePath(args.at(command + 1)));
}

}

class JobProcessPool final : public QObject {
public:
    static JobProcessPool &instance()
    {
        // Jobs can be application children destroyed after an application-owned
        // pool. Keep this one bounded module scheduler alive through teardown.
        static auto *pool = new JobProcessPool(nullptr);
        return *pool;
    }
    bool admit(const QString &key, QString *error);
    void add(OwnedJobProcess *process) { active.append(process); }
    void remove(OwnedJobProcess *process);
    void schedule();
private:
    explicit JobProcessPool(QObject *parent);
    void advance();
    QList<OwnedJobProcess *> active;
    QTimer timer;
};

// This controller may outlive its requesting widget/job. Destruction therefore
// does not make QProcess synchronously wait on the GUI thread, and the exact
// process-group authority remains alive until all owned workers have stopped.
class OwnedJobProcess final : public QObject {
public:
    explicit OwnedJobProcess(QString resource, QObject *parent) : QObject(parent), key(std::move(resource))
    {
        JobProcessPool::instance().add(this);
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
            if (orphaned) discardOutput(QProcess::StandardOutput);
        });
        connect(&process, &QProcess::readyReadStandardError, this, [this] {
            if (orphaned) discardOutput(QProcess::StandardError);
        });
        process.setChildProcessModifier([] { if (::setsid() < 0) ::_exit(126); });
        connect(&process, &QProcess::started, this, [this] {
            group = process.processId();
            descriptor = static_cast<int>(::syscall(SYS_pidfd_open, group, 0));
        });
        connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            exited = true;
            exitCode = code;
            exitStatus = status;
        });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
            // The event loop cannot continue asynchronous reaping after exit.
            // Retain exact group authority and stop remaining workers now.
            if (!stopped) {
                signalGroup(SIGKILL);
                if (process.state() != QProcess::NotRunning) process.kill();
            }
        });
    }
    ~OwnedJobProcess() override { JobProcessPool::instance().remove(this); if (descriptor >= 0) ::close(descriptor); }
    void requestStop()
    {
        if (stopped || elapsed.isValid()) return;
        elapsed.start();
        // Allow an already completed process's queued exit acknowledgement to
        // arrive before signalling. Cancellation remains pending in this interval.
        nextObservation = 25;
        JobProcessPool::instance().schedule();
    }
    void release()
    {
        setParent(nullptr);
        onStopped = {};
        orphaned = true;
        discardOutput(QProcess::StandardOutput);
        discardOutput(QProcess::StandardError);
        if (stopped) deleteLater();
        else requestStop();
    }
    QString key;
    QProcess process;
    friend class JobProcessPool;
    std::function<void()> onStopped;
    bool stopped = false;
    bool exited = false;
    int exitCode = 0;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
private:
    void discardOutput(QProcess::ProcessChannel channel)
    {
        const auto previous = process.readChannel();
        process.setReadChannel(channel);
        while (process.bytesAvailable() > 0)
            if (process.read(64 * 1024).isEmpty()) break;
        process.setReadChannel(previous);
    }
    bool signalGroup(int signal)
    {
        return descriptor >= 0
            && ::syscall(SYS_pidfd_send_signal, descriptor, signal, nullptr, kSignalProcessGroup) == 0;
    }
    void advance(GroupObservations &observations)
    {
        if (process.state() == QProcess::Starting) {
            // Exec can itself stall before QProcess emits started. Its owned
            // leader can still be killed, but no group authority is available
            // until the session has been established and observed.
            if (elapsed.elapsed() >= 2000) process.kill();
            return;
        }
        if (!termSent) {
            termSent = true;
            if (!signalGroup(SIGTERM) && process.state() != QProcess::NotRunning) process.terminate();
        }
        if (elapsed.elapsed() >= 2000 && !killSent) {
            killSent = true;
            signalGroup(SIGKILL);
            if (process.state() != QProcess::NotRunning) process.kill();
        }
        if (process.state() != QProcess::NotRunning || observations.state(group, descriptor) != GroupState::Stopped) return;
        stopped = true;
        if (descriptor >= 0) { ::close(descriptor); descriptor = -1; }
        QPointer<OwnedJobProcess> guard(this);
        JobProcessPool::instance().remove(this);
        if (!guard) return;
        if (orphaned) deleteLater();
        else if (onStopped) onStopped();
    }
    qint64 nextObservation = 25;
    int interval = 25;
    QElapsedTimer elapsed;
    qint64 group = 0;
    int descriptor = -1;
    bool termSent = false;
    bool killSent = false;
    bool orphaned = false;
};

JobProcessPool::JobProcessPool(QObject *parent) : QObject(parent)
{
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, this, [this] { advance(); });
}

namespace {
// Both Dolphin plugins may load separate copies of this implementation. Share
// admission as a Qt value on the application, while each module owns its cleanup.
constexpr auto kAdmissionsProperty = "_dwarfsctl_owned_jobs_v1";
}

bool JobProcessPool::admit(const QString &key, QString *error)
{
    auto *app = QCoreApplication::instance();
    Q_ASSERT(app && QThread::currentThread() == app->thread());
    const auto value = app->property(kAdmissionsProperty);
    if (value.isValid() && value.metaType() != QMetaType::fromType<QStringList>()) {
        *error = i18n("Operation tracking state is invalid. Restart the application before trying again.");
        qCWarning(DWARFS_DOLPHIN_LOG) << *error << value.metaType().name();
        return false;
    }
    auto keys = value.toStringList();
    if (keys.size() >= 8 || (!key.isEmpty() && keys.contains(key))) {
        *error = i18n("Another operation is still using this image, or the operation limit has been reached. Try again after it finishes.");
        return false;
    }
    keys.append(key);
    app->setProperty(kAdmissionsProperty, keys);
    return true;
}

void JobProcessPool::remove(OwnedJobProcess *process)
{
    if (active.removeAll(process) == 0) return;
    auto *app = QCoreApplication::instance();
    if (!app) return;
    Q_ASSERT(QThread::currentThread() == app->thread());
    const auto value = app->property(kAdmissionsProperty);
    if (value.metaType() != QMetaType::fromType<QStringList>()) {
        qCWarning(DWARFS_DOLPHIN_LOG) << "Cannot release invalid operation tracking state";
        return;
    }
    auto keys = value.toStringList();
    if (!keys.removeOne(process->key)) {
        qCWarning(DWARFS_DOLPHIN_LOG) << "Missing operation admission during release";
        return;
    }
    app->setProperty(kAdmissionsProperty, keys);
}

void JobProcessPool::schedule()
{
    if (!QCoreApplication::instance() || !QAbstractEventDispatcher::instance()) return;
    int delay = 1000;
    bool pending = false;
    for (auto *process : active) {
        if (!process->elapsed.isValid()) continue;
        pending = true;
        delay = qMin(delay, int(qMax(qint64(1), process->nextObservation - process->elapsed.elapsed())));
    }
    if (pending) {
        if (!timer.isActive() || timer.remainingTime() > delay) timer.start(delay);
    } else timer.stop();
}

void JobProcessPool::advance()
{
    GroupObservations observations;
    QList<QPointer<OwnedJobProcess>> due;
    for (auto *process : active) {
        observations.groups.insert(process->group, GroupState::Stopped);
        if (process->elapsed.isValid() && process->elapsed.elapsed() >= process->nextObservation)
            due.append(process);
    }
    // Completion callbacks may destroy this or another job/controller.
    for (auto process : due) {
        if (!process || process->stopped) continue;
        if (process->elapsed.elapsed() >= 2000) process->interval = qMin(1000, process->interval * 2);
        process->nextObservation = process->elapsed.elapsed() + process->interval;
        process->advance(observations);
    }
    schedule();
}

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
    setCapabilities(KJob::Killable);

    m_timeout.setSingleShot(true);

    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        if (!m_resultEmitted && !m_stopping) {
            qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl timed out" << m_req.args;
            setError(KJob::UserDefinedError);
            m_errorString = i18n("Timed out");
            setErrorText(i18n("Operation exceeded configured timeout"));
            // Provide structured error info for the Status dialog.
            m_envelope.ok = false;
            m_envelope.has_error = true;
            m_envelope.error.kind = QStringLiteral("timeout");
            m_envelope.error.message = i18n("dwarfsctl did not finish before the timeout");
            attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
            m_hasEnvelope = true;
            finishWhenStopped();
        }
    });
}

void DwarfsctlJob::initializeProcess(const QString &resource)
{
    m_owned = new OwnedJobProcess(resource, this);
    m_proc = &m_owned->process;
    m_owned->onStopped = [this] { completeAfterCleanup(); };
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_stopping || m_req.responseLimit > 0) readOutput(QProcess::StandardOutput);
    });
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        if (!m_stopping || m_req.responseLimit > 0) readOutput(QProcess::StandardError);
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (m_stopping) return;
        // Most common failure in practice: Dolphin launched from a desktop entry
        // does not inherit the same PATH as an interactive shell, so dwarfsctl
        // (often installed in ~/.local/bin) cannot be found/executed.
        setError(KJob::UserDefinedError);
        m_errorString = m_proc->errorString();
        m_envelope.ok = false;
        m_envelope.has_error = true;
        const bool failedToStart = e == QProcess::FailedToStart;
        m_envelope.error.kind = failedToStart ? QStringLiteral("spawn") : QStringLiteral("process");
        if (failedToStart) {
            m_envelope.error.message = i18n("Failed to start '%1': %2", m_proc->program(), m_proc->errorString());
        } else if (e == QProcess::ReadError) {
            m_envelope.error.message = i18n("Could not read the operation's output: %1", m_proc->errorString());
        } else if (e == QProcess::WriteError) {
            m_envelope.error.message = i18n("Could not write to the operation: %1", m_proc->errorString());
        } else {
            m_envelope.error.message = i18n("The operation stopped unexpectedly: %1", m_proc->errorString());
        }
        attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
        m_hasEnvelope = true;

        setErrorText(m_envelope.error.message);
        qCWarning(DWARFS_DOLPHIN_LOG) << "dwarfsctl process error" << e << m_proc->program() << m_proc->errorString();
        finishWhenStopped();
    });
    connect(m_proc, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        Q_UNUSED(exitStatus)
        if (m_stopping) return;
        m_timeout.stop();

        // A progress consumer may close its owning dialog while output drains.
        const QPointer<DwarfsctlJob> guard(this);
        readOutput(QProcess::StandardOutput);
        if (!guard) return;
        readOutput(QProcess::StandardError);
        if (!guard) return;

        if (error() != 0) {
            // errorOccurred already set an error and emitted result.
            return;
        }

        resolveOutput(exitCode, parseEnvelope(m_stdout));
        finishWhenStopped();
    });

}

void DwarfsctlJob::readOutput(QProcess::ProcessChannel channel)
{
    const auto previous = m_proc->readChannel();
    m_proc->setReadChannel(channel);
    while (m_proc->bytesAvailable() > 0) {
        const auto chunk = m_proc->read(64 * 1024);
        if (chunk.isEmpty()) break;
        auto &target = channel == QProcess::StandardOutput ? m_stdout : m_stderr;
        if (!captureOutput(target, chunk) || m_stopping || channel != QProcess::StandardError) continue;
        m_progressBuffer += chunk;
        int newline;
        while ((newline = m_progressBuffer.indexOf('\n')) >= 0) {
            const auto obj = QJsonDocument::fromJson(m_progressBuffer.left(newline)).object();
            m_progressBuffer.remove(0, newline + 1);
            if (obj.value(QStringLiteral("event")).toString() == QLatin1String("build_progress")) {
                const QPointer<DwarfsctlJob> guard(this);
                Q_EMIT buildPhase(obj.value(QStringLiteral("phase")).toString());
                if (!guard) return;
            }
        }
        if (m_progressBuffer.size() > 65536) m_progressBuffer.clear();
    }
    m_proc->setReadChannel(previous);
}

bool DwarfsctlJob::captureOutput(QByteArray &target, const QByteArray &bytes)
{
    if (m_responseExceeded) return false;
    if (m_req.responseLimit > 0) {
        const auto remaining = qMax(qsizetype(0), m_req.responseLimit - m_stdout.size() - m_stderr.size());
        if (bytes.size() > remaining) {
            target += bytes.first(remaining);
            m_responseExceeded = true;
            m_progressBuffer.clear();
            m_envelope.details.insert(QStringLiteral("response_truncated"), true);
            if (error() == 0) {
                setError(KJob::UserDefinedError);
                m_errorString = i18n("The service response is too large.");
                setErrorText(m_errorString);
                m_envelope.ok = false;
                m_envelope.has_error = true;
                m_envelope.error.kind = QStringLiteral("protocol");
                m_envelope.error.message = m_errorString;
                m_hasEnvelope = true;
            }
            finishWhenStopped();
            return false;
        }
    }
    target += bytes;
    return true;
}

void DwarfsctlJob::resolveOutput(int exitCode, const ParseResult &parsed)
{
    if (m_req.outputFormat == OutputFormat::Text) {
        m_hasEnvelope = true;
        m_envelope.ok = exitCode == 0;
        if (exitCode != 0) {
            setError(KJob::UserDefinedError);
            m_envelope.has_error = true;
            m_envelope.error.kind = QStringLiteral("cli");
            m_envelope.error.message = m_stderr.isEmpty() ? QString::fromUtf8(m_stdout) : QString::fromUtf8(m_stderr);
            m_errorString = i18n("dwarfsctl failed");
            setErrorText(m_envelope.error.message);
        }
        attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
        return;
    }
    // One parse and one result classification for every observed CLI exit.
    if (parsed.parsed) {
        m_envelope = parsed.envelope;
        m_hasEnvelope = true;
        if (!m_envelope.ok && m_cancelRequested &&
            m_envelope.details.value(QStringLiteral("cancelled")) == QJsonValue(true)) {
            setError(KJob::KilledJobError);
            m_errorString = i18n("Cancelled");
            setErrorText(m_errorString);
        } else if (!m_envelope.ok) {
            setError(KJob::UserDefinedError);
            m_errorString = m_envelope.error.message.isEmpty() ? i18n("Operation failed") : m_envelope.error.message;
            setErrorText(m_envelope.error.message.isEmpty() ? QString::fromUtf8(m_stderr) : m_envelope.error.message);
        } else if (exitCode != 0) {
            setError(KJob::UserDefinedError);
            m_errorString = i18n("dwarfsctl failed");
            setErrorText(m_stderr.isEmpty() ? QString::fromUtf8(m_stdout) : QString::fromUtf8(m_stderr));
        }
    } else {
        setError(KJob::UserDefinedError);
        m_envelope.ok = false;
        m_envelope.has_error = true;
        m_envelope.error.kind = exitCode != 0 ? QStringLiteral("cli") : QStringLiteral("protocol");
        m_envelope.error.message = exitCode != 0
            ? (m_stderr.isEmpty() ? QString::fromUtf8(m_stdout) : QString::fromUtf8(m_stderr))
            : parsed.error_message;
        m_errorString = exitCode != 0 ? i18n("dwarfsctl failed") : i18n("Failed to parse dwarfsctl JSON output");
        setErrorText(m_envelope.error.message);
        m_hasEnvelope = true;
    }
    attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
}

void DwarfsctlJob::finishWhenStopped()
{
    if (m_resultEmitted) return;
    // An observed terminal result takes precedence over later cancellation.
    m_terminalPending = true;
    m_stopping = true;
    m_timeout.stop();
    m_owned->requestStop();
}

void DwarfsctlJob::completeAfterCleanup()
{
    if (m_resultEmitted) return;
    readOutput(QProcess::StandardOutput);
    readOutput(QProcess::StandardError);
    if (m_cancelRequested && !m_terminalPending) {
        // Buffered output alone cannot prove exit success. A complete envelope
        // and an observed normal exit do provide a stronger terminal result.
        const auto parsed = parseEnvelope(m_stdout);
        if (m_owned->exited && m_owned->exitStatus == QProcess::NormalExit
            && (parsed.parsed || m_req.outputFormat == OutputFormat::Text)) {
            resolveOutput(m_owned->exitCode, parsed);
        } else {
            setError(KJob::KilledJobError);
            m_errorString = i18n("Cancelled");
            setErrorText(m_errorString);
        }
    }
    attachStdIoToEnvelope(&m_envelope, m_stdout, m_stderr);
    m_terminalPending = false;
    m_resultEmitted = true;
    const QPointer<DwarfsctlJob> guard(this);
    if (error() == 0 && m_envelope.ok) Q_EMIT infoMessage(this, i18n("Completed"));
    if (guard) emitResult();
}

DwarfsctlJob::~DwarfsctlJob()
{
    m_timeout.stop();
    if (m_owned) {
        m_proc->disconnect(this);
        m_owned->release();
    }
}

void DwarfsctlJob::start()
{
    if (m_started || m_resultEmitted) return;
    m_started = true;
    auto *app = QCoreApplication::instance();
    QString admissionError;
    const auto resource = readResource(m_req.args);
    const bool applicationThread = app && QThread::currentThread() == app->thread();
    if (!applicationThread || !JobProcessPool::instance().admit(resource, &admissionError)) {
        setError(KJob::UserDefinedError);
        m_errorString = applicationThread ? admissionError : i18n("Operations must start on the application thread.");
        setErrorText(m_errorString);
        m_envelope.ok = false;
        m_envelope.has_error = true;
        m_envelope.error.kind = applicationThread ? QStringLiteral("busy") : QStringLiteral("process");
        m_envelope.error.message = m_errorString;
        m_hasEnvelope = true;
        m_resultEmitted = true;
        QMetaObject::invokeMethod(this, [this] { emitResult(); }, Qt::QueuedConnection);
        return;
    }
    // Use a real KJob UI delegate so Dolphin presents a standard progress UI
    // (with cancellation) for long-running operations like builds.
    // KF6 API expects both flags and an optional window pointer.
    // We pass nullptr here; the job UI will still be shown and parented sensibly by KDE.
    if (m_req.show_ui) {
        setUiDelegate(new KDialogJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, nullptr));
    }

    initializeProcess(resource);
    const QPointer<DwarfsctlJob> guard(this);
    m_stopping = false;
    if (m_req.show_ui && !m_req.title.isEmpty()) {
        const QString cmdDisplay = QStringLiteral("%1 %2").arg(DwarfsctlRunner::dwarfsctlProgram(), m_req.args.join(QLatin1Char(' ')));
        const QString timeoutDisplay = m_req.timeout_ms > 0 ? QString::number(m_req.timeout_ms) + QStringLiteral(" ms") : i18n("(none)");
        Q_EMIT description(this,
                         m_req.title,
                         QPair<QString, QString>(i18n("Command"), cmdDisplay),
                         QPair<QString, QString>(i18n("Timeout"), timeoutDisplay));
        if (!guard) return;
    }

    Q_EMIT infoMessage(this, i18n("Starting…"));
    if (!guard) return;

    const QString program = DwarfsctlRunner::dwarfsctlProgram();
    qCInfo(DWARFS_DOLPHIN_LOG) << "running dwarfsctl" << program << m_req.args;

    m_proc->setProgram(program);
    m_proc->setArguments(m_req.args);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    m_stdout.clear();
    m_stderr.clear();

    m_proc->start();

    if (m_req.timeout_ms > 0) {
        m_timeout.start(m_req.timeout_ms);
    }
}

bool DwarfsctlJob::doKill()
{
    if (m_resultEmitted || m_terminalPending || m_cancelRequested) return false;
    qCInfo(DWARFS_DOLPHIN_LOG) << "cancelling dwarfsctl" << m_req.args;
    m_cancelRequested = true;
    m_stopping = true;
    m_timeout.stop();
    if (!m_owned) {
        m_started = true;
        m_resultEmitted = true;
        setError(KJob::KilledJobError);
        m_errorString = i18n("Cancelled");
        setErrorText(m_errorString);
        QMetaObject::invokeMethod(this, [this] { emitResult(); }, Qt::QueuedConnection);
        return false;
    }
    m_owned->requestStop();
    // KJob's synchronous true return would acknowledge cancellation now. Keep
    // it pending instead, then emit exactly one result after verified cleanup.
    return false;
}

QString DwarfsctlJob::errorString() const
{
    if (!m_errorString.isEmpty()) {
        return m_errorString;
    }
    return KJob::errorString();
}

QString DwarfsctlJob::textOutput() const
{
    return QString::fromUtf8(m_stdout);
}

Envelope DwarfsctlJob::envelope() const
{
    return m_envelope;
}

} // namespace dwarfsplugin
