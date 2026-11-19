// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_liveness.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

#include <cerrno>
#include <csignal>
#include <mutex>
#include <set>
#include <map>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

namespace dwarfsctl_tray {

QString unescapeMountinfoField(QString s)
{
    s.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    s.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    s.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    s.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return s;
}

namespace {
struct KernelMount { QString id, point, source; };
struct KernelTable { bool complete = false; QList<KernelMount> mounts; };
KernelTable readKernelTable(const QString &path, bool mountinfo)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    if (file.error() != QFile::NoError || bytes.isEmpty() || !bytes.endsWith('\n')) return {};
    KernelTable table;
    for (const auto &line : bytes.split('\n')) {
        if (line.isEmpty()) continue;
        const auto fields = QString::fromUtf8(line).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (mountinfo) {
            const int separator = fields.indexOf(QStringLiteral("-"));
            bool validId = false;
            if (fields.size() < 10 || separator < 6 || separator + 3 >= fields.size()
                || fields[0].toLongLong(&validId) <= 0 || !validId) return {};
            table.mounts.append({fields[0], unescapeMountinfoField(fields[4]), unescapeMountinfoField(fields[separator + 2])});
        } else {
            if (fields.size() < 6) return {};
            table.mounts.append({{}, unescapeMountinfoField(fields[1]), unescapeMountinfoField(fields[0])});
        }
    }
    table.complete = !table.mounts.isEmpty();
    return table;
}
KernelTable kernelTable()
{
    auto table = readKernelTable(QStringLiteral("/proc/self/mountinfo"), true);
    if (!table.complete) table = readKernelTable(QStringLiteral("/proc/self/mounts"), false);
    return table;
}
}

MountObservation observeMount(const QString &mountpoint)
{
    if (mountpoint.isEmpty()) return {};
    const auto table = kernelTable();
    if (!table.complete) return {};
    const auto wanted = QDir::cleanPath(QFileInfo(mountpoint).absoluteFilePath());
    const auto find = [&table](const QString &path) -> MountObservation {
        for (const auto &mount : table.mounts)
            if (QDir::cleanPath(mount.point) == path) return {MountPresence::Present, mount.source};
        return {MountPresence::Absent, {}};
    };
    auto observed = find(wanted);
    if (observed.presence == MountPresence::Present) return observed;
    int error = 0;
    const auto canonical = boundedCanonicalFilePath(wanted, &error);
    if (!canonical.isEmpty()) return find(canonical);
    if (error == ENOENT || error == ENOTDIR) return {MountPresence::Absent, {}};
    return {};
}

bool isMounted(const QString& mountpoint)
{
    return observeMount(mountpoint).presence == MountPresence::Present;
}

bool mountImageMatches(const QString& mountpoint, const QString& image)
{
    const auto observed = observeMount(mountpoint);
    return !image.isEmpty() && observed.presence == MountPresence::Present
        && QDir::cleanPath(observed.source) == QDir::cleanPath(image);
}

MountLiveness classifyStatfsResult(int rc, int err)
{
    if (rc == 0)
        return MountLiveness::Live;
    // ENOTCONN is the one errno that is evidence of the specific failure this
    // classification exists for: the FUSE daemon is gone, so requests cannot be
    // delivered. Other errors (ENOENT, EACCES, ...) say something about the
    // path, not about the daemon, so they are not treated as a dead mount.
    if (err == ENOTCONN)
        return MountLiveness::Dead;
    return MountLiveness::Unknown;
}

bool mountEntryIsUsable(bool listed, MountLiveness probed)
{
    if (!listed)
        return false;
    // Only positive evidence of a broken mount rejects it. A probe that could
    // not run is no evidence at all, and treating it as one would hide healthy
    // mounts on systems where the probe cannot fork.
    return probed != MountLiveness::Dead && probed != MountLiveness::Unresponsive;
}

QString mountLivenessName(MountLiveness liveness)
{
    switch (liveness) {
    case MountLiveness::Live:
        return QStringLiteral("live");
    case MountLiveness::Dead:
        return QStringLiteral("dead");
    case MountLiveness::Unresponsive:
        return QStringLiteral("unresponsive");
    case MountLiveness::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

namespace {
// Active and deferred children share one bounded budget. An unresolved request
// for the same path is reused, so repeated refreshes cannot add blocked children.
struct DeferredProbes {
    std::mutex mutex;
    std::map<pid_t, QString> children;
    std::set<QString> admitted;
};
DeferredProbes &deferredProbes()
{
    static DeferredProbes probes;
    return probes;
}
QString probeKey(const QString &path)
{
    // Kernel mount IDs distinguish replacement volumes at the same directory.
    // Descendants of one stalled mount share admission even across operations.
    QFile table(QStringLiteral("/proc/self/mountinfo"));
    QString key = QDir::cleanPath(path);
    qsizetype longest = -1;
    if (table.open(QIODevice::ReadOnly)) {
        for (const auto &line : table.readAll().split('\n')) {
            const auto fields = QString::fromUtf8(line).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (fields.size() < 5) continue;
            const auto point = unescapeMountinfoField(fields[4]);
            if ((path == point || path.startsWith(point == QLatin1String("/") ? point : point + QLatin1Char('/')))
                && point.size() > longest) {
                longest = point.size();
                // A lexical alias outside a FUSE mount can resolve into one.
                // Avoid assigning that unresolved alias to the entire ordinary
                // parent filesystem and suppressing its unrelated healthy paths.
                const int separator = fields.indexOf(QStringLiteral("-"));
                key = separator >= 0 && separator + 1 < fields.size()
                    && fields[separator + 1].startsWith(QLatin1String("fuse"))
                    ? QStringLiteral("mount:") + fields[0] : QDir::cleanPath(path);
            }
        }
    }
    return key;
}
bool admitProbe(const QString &key, bool *unresolved = nullptr)
{
    if (unresolved) *unresolved = false;
    auto &probes = deferredProbes();
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(probes.mutex);
            bool pending = false;
            for (auto it = probes.children.begin(); it != probes.children.end();) {
                const auto result = ::waitpid(it->first, nullptr, WNOHANG);
                if (result == it->first || (result < 0 && errno == ECHILD)) {
                    probes.admitted.erase(it->second);
                    it = probes.children.erase(it);
                } else {
                    if (it->second == key) pending = true;
                    ++it;
                }
            }
            if (pending) { if (unresolved) *unresolved = true; return false; }
            if (probes.admitted.size() < 4 && !probes.admitted.count(key)) {
                probes.admitted.insert(key);
                return true;
            }
            if (probes.children.size() >= 4) return false;
        }
        // Brief active probes can finish and release admission. Waiting for
        // admission is bounded too; no caller can create an unbounded queue.
        if (timer.elapsed() >= kMountLivenessProbeTimeoutMs + kMountLivenessReapGraceMs) return false;
        QThread::msleep(5);
    }
}
void finishProbe(const QString &key, pid_t deferred = 0)
{
    auto &probes = deferredProbes();
    std::lock_guard<std::mutex> lock(probes.mutex);
    if (deferred > 0) probes.children.emplace(deferred, key);
    else probes.admitted.erase(key);
}
// The caller exclusively owns pid until it transfers it to the deferred set.
// Monotonic time bounds both the request and post-SIGKILL collection.
bool awaitProbe(pid_t pid, int &status, bool &deferred)
{
    deferred = false;
    QElapsedTimer timer;
    timer.start();
    bool killed = false;
    for (;;) {
        const auto result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) return !killed;
        if (result < 0 && errno != EINTR) return false;
        if (!killed && timer.elapsed() >= kMountLivenessProbeTimeoutMs) {
            ::kill(pid, SIGKILL);
            killed = true;
            timer.restart();
        } else if (killed && timer.elapsed() >= kMountLivenessReapGraceMs) {
            deferred = true;
            return false;
        }
        QThread::msleep(5);
    }
}
}

QString boundedCanonicalFilePath(const QString &path, int *error)
{
    if (error) *error = EAGAIN;
    if (path.isEmpty()) return {};
    const auto key = probeKey(path);
    if (!admitProbe(key)) return {};
    const auto encoded = QFile::encodeName(path);
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) { finishProbe(key); return {}; }
    const auto pid = ::fork();
    if (pid == 0) {
        // Only syscalls and stack operations after fork. O_PATH follows aliases
        // without opening file contents; procfs reports the resolved kernel path.
        ::close(pipes[0]);
        const int fd = ::open(encoded.constData(), O_PATH | O_CLOEXEC);
        if (fd < 0) { const int failure = errno; (void)::write(pipes[1], &failure, sizeof(failure)); ::_exit(1); }
        char link[64] = "/proc/self/fd/";
        char digits[16]; int n = 0, value = fd;
        do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
        int at = 14;
        while (n) link[at++] = digits[--n];
        link[at] = '\0';
        char resolved[PATH_MAX];
        const auto size = ::readlink(link, resolved, sizeof(resolved));
        const int failure = size > 0 && size < PATH_MAX ? 0 : errno;
        (void)::write(pipes[1], &failure, sizeof(failure));
        if (size > 0 && size < PATH_MAX) (void)::write(pipes[1], resolved, size);
        ::_exit(0);
    }
    ::close(pipes[1]);
    if (pid < 0) { ::close(pipes[0]); finishProbe(key); return {}; }
    int status = 0; bool deferred = false;
    const bool finished = awaitProbe(pid, status, deferred);
    char resolved[PATH_MAX];
    int failure = EAGAIN;
    const bool received = finished && ::read(pipes[0], &failure, sizeof(failure)) == sizeof(failure);
    const auto size = received && failure == 0 ? ::read(pipes[0], resolved, sizeof(resolved)) : -1;
    if (error) *error = size > 0 ? 0 : failure == 0 ? EAGAIN : failure;
    ::close(pipes[0]);
    finishProbe(key, deferred ? pid : 0);
    return size > 0 ? QFile::decodeName(QByteArray(resolved, size)) : QString{};
}

QByteArray boundedRegularFileRead(const QString &path, int maximumBytes)
{
    if (path.isEmpty() || maximumBytes <= 0 || maximumBytes > 4096) return {};
    const auto key = probeKey(path);
    if (!admitProbe(key)) return {};
    const auto encoded = QFile::encodeName(path);
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) { finishProbe(key); return {}; }
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(pipes[0]);
        const int fd = ::open(encoded.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) ::_exit(1);
        struct stat info {};
        if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > maximumBytes) ::_exit(1);
        char bytes[4097]; int used = 0;
        for (;;) {
            const auto count = ::read(fd, bytes + used, maximumBytes + 1 - used);
            if (count < 0) { if (errno == EINTR) continue; ::_exit(1); }
            if (!count) break;
            used += count;
            if (used > maximumBytes) ::_exit(1);
        }
        if (used) (void)::write(pipes[1], bytes, used);
        ::_exit(0);
    }
    ::close(pipes[1]);
    if (pid < 0) { ::close(pipes[0]); finishProbe(key); return {}; }
    int status = 0; bool deferred = false;
    const bool finished = awaitProbe(pid, status, deferred);
    char bytes[4096];
    const auto size = finished ? ::read(pipes[0], bytes, maximumBytes) : -1;
    ::close(pipes[0]); finishProbe(key, deferred ? pid : 0);
    return size > 0 ? QByteArray(bytes, size) : QByteArray{};
}

bool boundedFileStat(const QString &path, struct stat *result, int *error)
{
    if (error) *error = EAGAIN;
    if (path.isEmpty() || !result) return false;
    const auto key = probeKey(path);
    if (!admitProbe(key)) return false;
    const auto encoded = QFile::encodeName(path);
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) { finishProbe(key); return false; }
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(pipes[0]);
        struct stat value {};
        const int rc = ::stat(encoded.constData(), &value);
        const int statError = rc == 0 ? 0 : errno;
        (void)::write(pipes[1], &statError, sizeof(statError));
        if (rc == 0) (void)::write(pipes[1], &value, sizeof(value));
        ::_exit(0);
    }
    ::close(pipes[1]);
    if (pid < 0) { ::close(pipes[0]); finishProbe(key); return false; }
    int status = 0; bool deferred = false;
    const bool finished = awaitProbe(pid, status, deferred);
    int statError = EAGAIN;
    const bool received = finished && ::read(pipes[0], &statError, sizeof(statError)) == sizeof(statError);
    const auto size = received && statError == 0 ? ::read(pipes[0], result, sizeof(*result)) : -1;
    if (error) *error = received ? statError : EAGAIN;
    ::close(pipes[0]);
    finishProbe(key, deferred ? pid : 0);
    return size == sizeof(*result);
}

MountLiveness probeMountLiveness(const QString& mountpoint)
{
    if (mountpoint.isEmpty()) return MountLiveness::Unknown;
    const auto key = probeKey(mountpoint);
    bool unresolved = false;
    if (!admitProbe(key, &unresolved)) return unresolved ? MountLiveness::Unresponsive : MountLiveness::Unknown;
    const QByteArray path = QFile::encodeName(mountpoint);
    const pid_t pid = ::fork();
    if (pid < 0) { finishProbe(key); return MountLiveness::Unknown; }
    if (pid == 0) {
        struct statfs st {};
        const int rc = ::statfs(path.constData(), &st);
        ::_exit(rc == 0 ? 0 : (errno & 0xff));
    }
    int status = 0; bool deferred = false;
    const bool finished = awaitProbe(pid, status, deferred);
    finishProbe(key, deferred ? pid : 0);
    if (!finished) return MountLiveness::Unresponsive;
    if (!WIFEXITED(status)) return MountLiveness::Unknown;
    const int err = WEXITSTATUS(status);
    return classifyStatfsResult(err == 0 ? 0 : -1, err);
}

bool mountsUsable(const QString& mountpoint)
{
    if (!isMounted(mountpoint))
        return false;

    const auto liveness = probeMountLiveness(mountpoint);
    if (liveness == MountLiveness::Dead || liveness == MountLiveness::Unresponsive)
        return false;

    // Live, or a probe that could not run. The second case keeps the answer the
    // mount table alone gives: a probe that never ran is no evidence that a
    // mount is broken, and treating it as such would hide healthy mounts.
    return true;
}

} // namespace dwarfsctl_tray
