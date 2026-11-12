// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctl_cat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <functional>

namespace DwarfsKio {

namespace {

QString programPath()
{
    const QString override = qEnvironmentVariable("DWARFSCTL_BIN");
    if (!override.isEmpty()) {
        return override;
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("dwarfsctl"));
    return found.isEmpty() ? QStringLiteral("dwarfsctl") : found;
}

void backendError(QProcess &process, int exitCode, QString *kind, QString *message)
{
    const QByteArray stdoutBytes = process.readAllStandardOutput().trimmed();
    const QJsonObject envelope = QJsonDocument::fromJson(stdoutBytes).object();
    const QJsonObject error = envelope.value(QStringLiteral("error")).toObject();
    *kind = error.value(QStringLiteral("kind")).toString(QStringLiteral("BackendFailed"));
    *message = error.value(QStringLiteral("message"))
                   .toString(QStringLiteral("dwarfsctl failed (exit code %1)").arg(exitCode));
}

} // namespace

MaterialisedFile materialise(const QString &imagePath, const QString &innerPath)
{
    MaterialisedFile out;

    // A private temporary directory, created 0700 by QTemporaryDir. The backend
    // writes the extracted file here; the directory is what gets removed on
    // release.
    auto *dir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/dwarfs-kio-XXXXXX"));
    if (!dir->isValid()) {
        delete dir;
        out.errorKind = QStringLiteral("TempDirFailed");
        out.error = QStringLiteral("could not create a temporary directory to read the file into");
        return out;
    }
    // Tighten to the owner only: the file may contain anything.
    dir->setAutoRemove(true);

    const QString dest = dir->filePath(QStringLiteral("content"));
    const QString program = programPath();

    // argv-only, no shell.
    QProcess process;
    process.setProgram(program);
    process.setArguments({QStringLiteral("cat"), imagePath, innerPath, dest,
                          QStringLiteral("--force")});
    process.start();

    if (!process.waitForStarted(10000)) {
        out.errorKind = QStringLiteral("DependencyMissing");
        out.error = QStringLiteral("could not start %1: %2").arg(program, process.errorString());
        delete dir;
        return out;
    }

    // Generous: materialising a large file from a compressed image costs real time.
    if (!process.waitForFinished(600000)) {
        process.kill();
        process.waitForFinished(5000);
        delete dir;
        out.errorKind = QStringLiteral("Timeout");
        out.error = QStringLiteral("dwarfsctl cat did not finish in time");
        return out;
    }

    const int exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;

    // Carry the backend's own message through: it names the artifact and the cause,
    // which is more useful than anything this layer could invent.
    if (exitCode != 0) {
        backendError(process, exitCode, &out.errorKind, &out.error);
        delete dir;
        return out;
    }

    // The post-condition is the FILE, not the exit status. The backend already
    // checks this, but the bytes reaching here are the thing being served, so
    // confirm the file is really there before reporting success.
    const QFileInfo info(dest);
    if (!info.exists() || !info.isFile()) {
        out.errorKind = QStringLiteral("EmptyOutput");
        out.error = QStringLiteral("dwarfsctl reported success but produced no file at the "
                                   "expected location");
        delete dir;
        return out;
    }

    out.ok = true;
    out.localPath = dest;
    out.size = static_cast<quint64>(info.size());
    // Keep the directory alive until release(); it is owned by the caller now.
    return out;
}

CopyOutResult copyOutToLocal(const QString &imagePath,
                             const QString &innerPath,
                             const QString &destination,
                             bool overwrite,
                             const std::function<bool()> &cancelled)
{
    CopyOutResult out;
    const QFileInfo existing(destination);
    if (existing.exists() && existing.isDir()) {
        out.errorKind = QStringLiteral("DestinationIsDirectory");
        out.error = QStringLiteral("the destination is a directory: %1").arg(destination);
        return out;
    }
    if (existing.exists() && !overwrite) {
        out.errorKind = QStringLiteral("DestinationExists");
        out.error = QStringLiteral("the destination already exists: %1").arg(destination);
        return out;
    }

    QProcess process;
    process.setProgram(programPath());
    QStringList arguments{QStringLiteral("cat"), imagePath, innerPath, destination};
    if (overwrite) {
        arguments.append(QStringLiteral("--force"));
    }
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(10000)) {
        out.errorKind = QStringLiteral("DependencyMissing");
        out.error = QStringLiteral("could not start %1: %2")
                        .arg(process.program(), process.errorString());
        return out;
    }

    while (!process.waitForFinished(100)) {
        if (cancelled && cancelled()) {
            process.kill();
            process.waitForFinished(5000);
            out.errorKind = QStringLiteral("Cancelled");
            out.error = QStringLiteral("copy was cancelled");
            return out;
        }
    }

    const int exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    if (exitCode != 0) {
        backendError(process, exitCode, &out.errorKind, &out.error);
        return out;
    }

    const QFileInfo published(destination);
    if (!published.exists() || !published.isFile()) {
        out.errorKind = QStringLiteral("EmptyOutput");
        out.error = QStringLiteral("dwarfsctl reported success but did not publish %1")
                        .arg(destination);
        return out;
    }
    out.ok = true;
    out.size = static_cast<quint64>(published.size());
    return out;
}

void releaseMaterialised(const MaterialisedFile &file)
{
    if (file.localPath.isEmpty()) {
        return;
    }
    // The file lives in a directory created for it. Removing that directory
    // removes the content file with it.
    //
    // QDir is deliberately non-const: removeRecursively() is a non-const member,
    // so a const QDir does not compile.
    QDir parent = QFileInfo(file.localPath).dir();
    if (parent.exists()) {
        parent.removeRecursively();
    }
}

bool readWholeFile(const QString &path, quint64 size, QByteArray *out, quint64 maxBytes)
{
    if (out == nullptr) {
        return false;
    }
    // Refuse an over-large file rather than reading it and refusing later, and
    // refuse rather than truncate: a partial file reported as success is
    // indistinguishable from a complete one to the caller.
    if (maxBytes != 0 && size > maxBytes) {
        return false;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    // Read one byte past the declared size so a file that grew since the stat is
    // detected rather than silently truncated.
    QByteArray data = f.read(static_cast<qint64>(size) + 1);
    if (static_cast<quint64>(data.size()) != size) {
        // Size disagreed with what the backend reported. Do not pretend otherwise.
        return false;
    }
    *out = data;
    return true;
}

bool streamFile(const QString &path, const std::function<bool(const QByteArray &)> &emit)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }

    // Bounded buffer: a DwarFS image can hold a file far larger than anyone wants
    // in memory, and Dolphin will happily ask for all of it. Chunks keep the
    // resident set flat regardless of file size.
    //
    // The read decides the end of the data, never a size the device reports:
    // QFile::atEnd() trusts the reported size, and a filesystem that reports
    // size 0 for non-empty content (procfs and other pseudo-files) would end
    // this loop before a single chunk. An empty read is a clean end only when
    // the device reports no error.
    constexpr qint64 kChunk = 256 * 1024;
    for (;;) {
        const QByteArray chunk = f.read(kChunk);
        if (chunk.isEmpty()) {
            return f.error() == QFileDevice::NoError;
        }
        if (!emit(chunk)) {
            return true; // caller stopped early; not a failure
        }
    }
}

} // namespace DwarfsKio
