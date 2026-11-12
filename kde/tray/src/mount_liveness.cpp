// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_liveness.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

#include <cerrno>
#include <csignal>
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

bool isMounted(const QString& mountpoint)
{
    // Prefer a raw cleaned path match first, then fall back to canonical path
    // comparison when available.
    //
    // Rationale:
    // - Canonicalization may differ across systems (/run vs /var/run)
    // - canonicalPath() can return empty in edge cases
    const auto mpWantClean = QDir::cleanPath(mountpoint);
    // canonicalFilePath() can be empty in edge cases; treat it as a best-effort fallback.
    const auto mpWantCanon = QFileInfo(mpWantClean).canonicalFilePath();

    auto isMountPointByStat = [](const QString& path) -> bool {
        if (path.isEmpty())
            return false;
        const QFileInfo fi(path);
        if (!fi.exists() || !fi.isDir())
            return false;
        const QFileInfo parentFi(fi.dir().absolutePath());
        if (!parentFi.exists())
            return false;

        struct stat stMp {};
        struct stat stParent {};
        const auto mpBytes = QFile::encodeName(fi.absoluteFilePath());
        const auto parentBytes = QFile::encodeName(parentFi.absoluteFilePath());
        if (::stat(mpBytes.constData(), &stMp) != 0)
            return false;
        if (::stat(parentBytes.constData(), &stParent) != 0)
            return false;

        // Standard mount-point heuristic: different device OR same inode as parent (root).
        return (stMp.st_dev != stParent.st_dev) || (stMp.st_ino == stParent.st_ino);
    };

    auto matchMountpoint = [&](const QString& mpRaw) -> bool {
        const auto mp = unescapeMountinfoField(mpRaw);
        const auto mpClean = QDir::cleanPath(mp);
        if (mpClean == mpWantClean)
            return true;
        if (!mpWantCanon.isEmpty()) {
            const auto mpCanon = QFileInfo(mpClean).canonicalFilePath();
            if (!mpCanon.isEmpty() && mpCanon == mpWantCanon)
                return true;
        }
        return false;
    };

    // Primary: /proc/self/mountinfo.
    //
    // The contents are read in one call and split, never line by line behind an
    // `atEnd()` gate. That gate is wrong here in a way nothing else reveals:
    // procfs reports st_size == 0 for every file, and QFile::atEnd() decides end
    // of data from that reported size, so an atEnd()-gated loop over
    // /proc/self/mountinfo reads zero lines -- and every path, including "/",
    // answered "not mounted" no matter what the table said. End of data is
    // decided by the read itself; the C++ reader in cpp/cli/diagnostics.cpp
    // follows the same rule.
    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (f.open(QIODevice::ReadOnly)) {
        const auto table = f.readAll();
        const auto lines = table.split('\n');
        for (const auto& rawLine : lines) {
            const auto line = QString::fromUtf8(rawLine).trimmed();
            if (line.isEmpty())
                continue;
            const auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.size() < 10)
                continue;
            if (!matchMountpoint(parts.value(4)))
                continue;

            // The mountpoint matched, so it is in the mount table. The
            // filesystem type is deliberately not used to decide this: a dwarfs
            // image can be mounted through different FUSE drivers across
            // versions, and a mismatch there must not read as "not mounted".
            // Callers that need the type read it from status, which reports it.
            //
            // This says nothing about whether the mount still works. A dead FUSE
            // daemon leaves its table entry behind, so a caller deciding whether
            // a path is usable must use mountsUsable() instead.
            return true;
        }

        // If mountinfo is readable and we didn't find it, it isn't mounted.
        return false;
    }

    // Fallback 1: /proc/self/mounts. Same rule: the read decides the end of the
    // data, never the size the kernel reports.
    QFile f2(QStringLiteral("/proc/self/mounts"));
    if (f2.open(QIODevice::ReadOnly)) {
        const auto table = f2.readAll();
        const auto lines = table.split('\n');
        for (const auto& rawLine : lines) {
            const auto line = QString::fromUtf8(rawLine).trimmed();
            if (line.isEmpty())
                continue;
            const auto parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.size() < 3)
                continue;
            if (matchMountpoint(parts.value(1)))
                return true;
        }

        return false;
    }

    // Fallback 2: stat() heuristic.
    return isMountPointByStat(mpWantClean);
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

MountLiveness probeMountLiveness(const QString& mountpoint)
{
    // The pid of a previous probe that could not be reaped while it was blocked
    // in the kernel. Tracked explicitly so the reap below can never collect a
    // child belonging to someone else -- `waitpid(-1)` would sit on QProcess
    // children too, which the tray starts elsewhere in this same process.
    static pid_t s_unreapedProbePid = -1;

    if (mountpoint.isEmpty())
        return MountLiveness::Unknown;

    if (s_unreapedProbePid > 0) {
        const pid_t r = ::waitpid(s_unreapedProbePid, nullptr, WNOHANG);
        if (r == s_unreapedProbePid || r < 0)
            s_unreapedProbePid = -1;
    }

    const QByteArray path = QFile::encodeName(mountpoint);

    const pid_t pid = ::fork();
    if (pid < 0)
        return MountLiveness::Unknown;

    if (pid == 0) {
        // Child. Only async-signal-safe calls after fork in a process that has
        // threads -- a raw syscall and _exit(), nothing else. _exit rather than
        // exit() so no Qt atexit handler runs here.
        struct statfs st {};
        const int rc = ::statfs(path.constData(), &st);
        ::_exit(rc == 0 ? 0 : (errno & 0xff));
    }

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kMountLivenessProbeTimeoutMs;
    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0)
            return MountLiveness::Unknown;
        if (QDateTime::currentMSecsSinceEpoch() >= deadline) {
            ::kill(pid, SIGKILL);
            // Bounded reap. A child blocked in the kernel cannot die until its
            // request returns, and the probe must answer promptly regardless;
            // whatever is left is collected at the top of the next probe.
            const qint64 reapDeadline = QDateTime::currentMSecsSinceEpoch() + kMountLivenessReapGraceMs;
            for (;;) {
                const pid_t r2 = ::waitpid(pid, &status, WNOHANG);
                if (r2 == pid || r2 < 0)
                    break;
                if (QDateTime::currentMSecsSinceEpoch() >= reapDeadline) {
                    s_unreapedProbePid = pid;
                    break;
                }
                QThread::msleep(5);
            }
            return MountLiveness::Unresponsive;
        }
        QThread::msleep(5);
    }

    if (!WIFEXITED(status))
        return MountLiveness::Unknown;
    // The child encodes the raw errno in its exit status; 0 means statfs()
    // succeeded, so the daemon served the request.
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
