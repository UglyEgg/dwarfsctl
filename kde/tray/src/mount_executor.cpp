#include <QCoreApplication>
// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mount_executor.h"
#include "mount_liveness.h"
#include "traysettings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include <sys/stat.h>
#include <unistd.h>

using namespace dwarfsctl_tray;

namespace {

static constexpr int kToolTimeoutMs = 30000;
static constexpr int kMountAppearTimeoutMs = 60000;
static constexpr int kUnmountDisappearTimeoutMs = 20000;


QJsonObject okEnvelope(const QString& command, const QString& image, const QJsonValue& details)
{
    QJsonObject root;
    root.insert(QStringLiteral("ok"), true);
    root.insert(QStringLiteral("command"), command);
    root.insert(QStringLiteral("image"), image.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(image));
    root.insert(QStringLiteral("details"), details);
    root.insert(QStringLiteral("warnings"), QJsonArray{});
    root.insert(QStringLiteral("warning_objects"), QJsonArray{});
    root.insert(QStringLiteral("error"), QJsonValue(QJsonValue::Null));
    return root;
}

QJsonObject errEnvelope(const QString& command, const QString& image, int code, const QString& kind, const QString& message)
{
    QJsonObject err;
    err.insert(QStringLiteral("code"), code);
    err.insert(QStringLiteral("kind"), kind);
    err.insert(QStringLiteral("message"), message);

    QJsonObject root;
    root.insert(QStringLiteral("ok"), false);
    root.insert(QStringLiteral("command"), command);
    root.insert(QStringLiteral("image"), image.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(image));
    root.insert(QStringLiteral("details"), QJsonValue(QJsonValue::Null));
    root.insert(QStringLiteral("warnings"), QJsonArray{});
    root.insert(QStringLiteral("warning_objects"), QJsonArray{});
    root.insert(QStringLiteral("error"), err);
    return root;
}

QString lockErrorToString(QLockFile::LockError e)
{
    switch (e) {
    case QLockFile::NoError:
        return QStringLiteral("NoError");
    case QLockFile::LockFailedError:
        return QStringLiteral("LockFailedError");
    case QLockFile::PermissionError:
        return QStringLiteral("PermissionError");
    case QLockFile::UnknownError:
        return QStringLiteral("UnknownError");
    }
    return QStringLiteral("UnknownError");
}


/// Session runtime directory, resolved the same way as `registryPath()` in
/// main.cpp.
///
/// Do NOT prefer the environment variable here. When the tray is started by
/// D-Bus activation, `XDG_RUNTIME_DIR` may be absent; falling back to
/// `XDG_STATE_HOME` would then point this file at a different registry than the
/// one main.cpp reads and writes, so the same process would maintain two
/// registry files and mounts made from Dolphin would be invisible to the tray.
/// Qt's RuntimeLocation (and the `/run/user/<uid>` fallback) always agrees with
/// main.cpp, because that is what main.cpp itself uses.
QString xdgRuntimeDir()
{
    QString run = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (run.isEmpty())
        run = QStringLiteral("/run/user/%1").arg(getuid());
    return run;
}

struct RegistryPaths {
    QString dir;
    QString registry;
    QString lock;
};

/// Resolve the tray-owned mount registry location.
///
/// Deliberately identical to `registryPath()` in main.cpp so one process cannot
/// disagree with itself about where mount state lives. The previous version
/// branched on `XDG_RUNTIME_DIR` but produced the same result either way, which
/// invited exactly that mistake.
RegistryPaths registryPaths()
{
    const auto dir = xdgRuntimeDir() + QStringLiteral("/dwarfsctl");
    return {dir, dir + QStringLiteral("/mounts.json"), dir + QStringLiteral("/mounts.lock")};
}

QJsonObject loadRegistryUnlocked(const RegistryPaths& p)
{
    QFile f(p.registry);
    if (!f.exists()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("mounts"), QJsonArray{});
        return obj;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        QJsonObject obj;
        obj.insert(QStringLiteral("mounts"), QJsonArray{});
        return obj;
    }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("mounts"), QJsonArray{});
        return obj;
    }
    auto obj = doc.object();
    if (!obj.value(QStringLiteral("mounts")).isArray())
        obj.insert(QStringLiteral("mounts"), QJsonArray{});
    return obj;
}

bool saveRegistryUnlocked(const RegistryPaths& p, const QJsonObject& obj, QString* err)
{
    QDir().mkpath(p.dir);
    QSaveFile sf(p.registry);
    if (!sf.open(QIODevice::WriteOnly)) {
        if (err)
            *err = QStringLiteral("failed to open registry for write: %1").arg(sf.errorString());
        return false;
    }
    QJsonDocument doc(obj);
    sf.write(doc.toJson(QJsonDocument::Indented));
    sf.write("\n");
    if (!sf.commit()) {
        if (err)
            *err = QStringLiteral("failed to commit registry: %1").arg(sf.errorString());
        return false;
    }
    return true;
}

QByteArray runTool(const QString& program, const QStringList& args, int timeoutMs, int* exitCode, QString* stdErr)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start();
    if (!p.waitForStarted(3000)) {
        if (exitCode)
            *exitCode = 127;
        if (stdErr)
            *stdErr = QStringLiteral("failed to start %1").arg(program);
        return {};
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        if (exitCode)
            *exitCode = 124;
        if (stdErr)
            *stdErr = QStringLiteral("tool timed out: %1").arg(program);
        return {};
    }
    if (exitCode)
        *exitCode = p.exitCode();
    if (stdErr)
        *stdErr = QString::fromUtf8(p.readAllStandardError());
    return p.readAllStandardOutput();
}

QString resolveTool(const QString& name)
{
    // Prefer PATH lookup.
    const auto path = QStandardPaths::findExecutable(name);
    return path;
}

/// Wait, bounded, for a mountpoint to leave the mount table.
bool waitForMountToDisappear(const QString& mountpoint, int timeoutMs)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (!isMounted(mountpoint))
            return true;
        QThread::msleep(50);
    }
    return !isMounted(mountpoint);
}

/// Clear a mount table entry whose daemon is gone.
///
/// `dwarfs` refuses to mount over such an entry ("Socket not connected"), so
/// without this the state is unrecoverable from the GUI and needs a shell.
/// `fusermount3 -u` clears the entry; the tool list matches the unmount path so
/// the same fallbacks apply.
///
/// Returns the tool's exit code, or 127 when no unmount tool exists.
int clearBrokenMountEntry(const QString& mountpoint, QString* err)
{
    QString tool = resolveTool(QStringLiteral("fusermount3"));
    QStringList args;
    if (!tool.isEmpty()) {
        args = {QStringLiteral("-u"), mountpoint};
    } else {
        tool = resolveTool(QStringLiteral("fusermount"));
        if (!tool.isEmpty()) {
            args = {QStringLiteral("-u"), mountpoint};
        } else {
            tool = resolveTool(QStringLiteral("umount"));
            if (!tool.isEmpty())
                args = {mountpoint};
        }
    }
    if (tool.isEmpty()) {
        if (err)
            *err = QStringLiteral("no unmount tool found (fusermount3/fusermount/umount)");
        return 127;
    }
    int ec = 0;
    runTool(tool, args, kToolTimeoutMs, &ec, err);
    return ec;
}

struct Identity {
    QString canonicalImage;
    QString hashHex;
    QString mountRoot;
    QString mountpoint;
    QString lockDir;
    QString lockPath;
};

Identity resolveIdentity(const QString& imagePath)
{
    QFileInfo fi(imagePath);
    const auto canonical = fi.canonicalFilePath();
    const auto bytes = canonical.toUtf8();
    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();

    const auto mountRoot = xdgRuntimeDir() + QStringLiteral("/dwarfs");
    const auto lockDir = mountRoot + QStringLiteral("/locks");

    QString stem = fi.completeBaseName();
    if (stem.isEmpty())
        stem = QStringLiteral("dwarfs");
    QString safe;
    safe.reserve(stem.size());
    for (const auto ch : stem) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_') || ch == QLatin1Char('.'))
            safe.push_back(ch);
        else
            safe.push_back(QLatin1Char('_'));
    }
    if (safe.isEmpty())
        safe = QStringLiteral("dwarfs");
    const auto shortHash = QString::fromLatin1(hash.left(8));
    const auto mountDir = safe + QStringLiteral("-") + shortHash;
    const auto mountpoint = QDir(mountRoot).filePath(mountDir);
    const auto lockPath = QDir(lockDir).filePath(QString::fromLatin1(hash) + QStringLiteral(".lock"));

    return {canonical, QString::fromLatin1(hash), mountRoot, mountpoint, lockDir, lockPath};
}

void removeManagedMountDirectory(const QString& mountpoint)
{
    const QString root = QDir::cleanPath(xdgRuntimeDir() + QStringLiteral("/dwarfs"));
    const QString path = QDir::cleanPath(mountpoint);
    const QFileInfo info(path);

    // Remove only a direct child of our runtime mount root, and only after it
    // has left the mount table. QDir::rmdir refuses non-empty directories.
    if (info.dir().absolutePath() != root || isMounted(path)) {
        return;
    }
    QDir(root).rmdir(info.fileName());
}

} // namespace

QByteArray MountExecutor::listMounts()
{
    const auto p = registryPaths();
    auto reg = loadRegistryUnlocked(p);
    const QJsonArray arr = reg.value(QStringLiteral("mounts")).toArray();

    // Registry should only contain mounts that actually exist. Clean out anything stale.
    bool cleaned = false;
    QJsonArray filtered;
    for (const auto& v : arr) {
        if (!v.isObject()) {
            cleaned = true;
            continue;
        }
        const auto o = v.toObject();
        const auto mp = o.value(QStringLiteral("mountpoint")).toString();
        // `mountsUsable` probes the mount, so an entry whose daemon died is
        // dropped here as stale. QFileInfo::exists() cannot do this: it answers
        // from the attribute cache, so it reports a dead mountpoint either way
        // depending on what was cached before the daemon died.
        if (!mp.isEmpty() && mountsUsable(mp)) {
            filtered.push_back(o);
        } else {
            cleaned = true;
            removeManagedMountDirectory(mp);
        }
    }
    if (cleaned) {
        reg.insert(QStringLiteral("mounts"), filtered);
        QString saveErr;
        (void)saveRegistryUnlocked(p, reg, &saveErr);
    }

    QJsonObject details;
    details.insert(QStringLiteral("count"), filtered.size());
    details.insert(QStringLiteral("cleaned_stale"), cleaned);
    details.insert(QStringLiteral("mounts"), filtered);

    const auto env = okEnvelope(QStringLiteral("mount_list"), QString(), details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

QByteArray MountExecutor::status(const QString& imagePath)
{
    const auto p = registryPaths();
    auto reg = loadRegistryUnlocked(p);
    const QJsonArray arr = reg.value(QStringLiteral("mounts")).toArray();

    // Clean stale entries before answering status so UI state doesn't get inverted.
    bool cleaned = false;
    QJsonArray filtered;
    for (const auto& v : arr) {
        if (!v.isObject()) {
            cleaned = true;
            continue;
        }
        const auto o = v.toObject();
        const auto mp = o.value(QStringLiteral("mountpoint")).toString();
        // `mountsUsable` probes the mount, so an entry whose daemon died is
        // dropped here as stale. QFileInfo::exists() cannot do this: it answers
        // from the attribute cache, so it reports a dead mountpoint either way
        // depending on what was cached before the daemon died.
        if (!mp.isEmpty() && mountsUsable(mp)) {
            filtered.push_back(o);
        } else {
            cleaned = true;
        }
    }
    if (cleaned) {
        reg.insert(QStringLiteral("mounts"), filtered);
        QString saveErr;
        (void)saveRegistryUnlocked(p, reg, &saveErr);
    }

    const QFileInfo fi(imagePath);
    const auto canonical = (fi.exists() ? fi.canonicalFilePath() : QString{});

    QString foundMp;
    for (const auto& v : filtered) {
        if (!v.isObject())
            continue;
        const auto o = v.toObject();
        const auto img = o.value(QStringLiteral("image")).toString();
        if (img == imagePath || (!canonical.isEmpty() && img == canonical)) {
            foundMp = o.value(QStringLiteral("mountpoint")).toString();
            break;
        }
    }

    // Usable, not merely listed: a registry entry can outlive its daemon, and
    // reporting `mounted: true` for a path that serves nothing is the same lie
    // that made a re-mount claim success. The probe runs only when an entry was
    // found, so an unmounted image costs nothing.
    const bool mounted = !foundMp.isEmpty() && mountsUsable(foundMp);

    QJsonObject details;
    details.insert(QStringLiteral("target"), imagePath);
    details.insert(QStringLiteral("found"), foundMp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(foundMp));
    details.insert(QStringLiteral("mounted"), mounted);
    details.insert(QStringLiteral("mountpoint"), foundMp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(foundMp));
    if (!foundMp.isEmpty())
        details.insert(QStringLiteral("mount_health"), mountLivenessName(probeMountLiveness(foundMp)));

    const auto env = okEnvelope(QStringLiteral("mount_status"), imagePath, details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

QByteArray MountExecutor::mount(const QString& imagePath)
{
    const auto ident = resolveIdentity(imagePath);
    if (ident.canonicalImage.isEmpty()) {
        const auto env = errEnvelope(QStringLiteral("mount"), imagePath, 1, QStringLiteral("Cli"), QStringLiteral("image not found"));
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }

    QDir().mkpath(ident.mountRoot);
    QDir().mkpath(ident.mountpoint);
    QDir().mkpath(ident.lockDir);

    QLockFile lock(ident.lockPath);
    // Keep locking robust against stale lockfiles (e.g., crashes). We build with
    // -DQT_NO_KEYWORDS, so avoid relying on keyword-like Qt constructs elsewhere.
    lock.setStaleLockTime(60 * 1000);
    if (!lock.tryLock(5000)) {
        // If the lock is stale (crash), remove and retry once.
        lock.removeStaleLockFile();
        if (!lock.tryLock(5000)) {
            const auto env = errEnvelope(
                QStringLiteral("mount"),
                imagePath,
                1,
                QStringLiteral("MountState"),
                QStringLiteral("failed to acquire mount lock (%1)").arg(lockErrorToString(lock.error())));
            return QJsonDocument(env).toJson(QJsonDocument::Indented);
        }
    }

    // A mount table entry is not the same as a working mount. A FUSE daemon
    // that died leaves its entry behind, and `dwarfs` then refuses to mount
    // over it, so reporting success here would strand the user with a path that
    // cannot be read and cannot be fixed from the GUI. Probe it instead, and
    // clear the leftover entry before mounting again.
    if (isMounted(ident.mountpoint)) {
        const auto liveness = probeMountLiveness(ident.mountpoint);
        if (liveness == MountLiveness::Live) {
            // Ensure registry upsert.
            const auto p = registryPaths();
            QLockFile regLock(p.lock);
            regLock.setStaleLockTime(0);
            regLock.tryLock(5000);
            auto obj = loadRegistryUnlocked(p);
            auto mounts = obj.value(QStringLiteral("mounts")).toArray();

            // upsert
            bool updated = false;
            for (int i = 0; i < mounts.size(); ++i) {
                const auto rec = mounts.at(i).toObject();
                if (rec.value(QStringLiteral("image")).toString() == ident.canonicalImage || rec.value(QStringLiteral("mountpoint")).toString() == ident.mountpoint) {
                    QJsonObject n;
                    n.insert(QStringLiteral("image"), ident.canonicalImage);
                    n.insert(QStringLiteral("mountpoint"), ident.mountpoint);
                    n.insert(QStringLiteral("mounted_at"), QDateTime::currentSecsSinceEpoch());
                    n.insert(QStringLiteral("pid"), static_cast<int>(getpid()));
                    mounts.replace(i, n);
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                QJsonObject n;
                n.insert(QStringLiteral("image"), ident.canonicalImage);
                n.insert(QStringLiteral("mountpoint"), ident.mountpoint);
                n.insert(QStringLiteral("mounted_at"), QDateTime::currentSecsSinceEpoch());
                n.insert(QStringLiteral("pid"), static_cast<int>(getpid()));
                mounts.append(n);
            }
            obj.insert(QStringLiteral("mounts"), mounts);
            QString saveErr;
            saveRegistryUnlocked(p, obj, &saveErr);

            // Still record for the UX "Recently Mounted" list.
            dwarfsctl_tray::recordRecentlyMountedImage(ident.canonicalImage);

            QJsonObject details;
            details.insert(QStringLiteral("mounted"), true);
            details.insert(QStringLiteral("mountpoint"), ident.mountpoint);
            details.insert(QStringLiteral("mount_health"), mountLivenessName(liveness));
            const auto env = okEnvelope(QStringLiteral("mount"), imagePath, details);
            return QJsonDocument(env).toJson(QJsonDocument::Indented);
        }

        // Dead or unresponsive: the entry is a leftover, not a mount. Clear it
        // so the mount below can proceed, and drop any registry record that
        // claims this mountpoint is live.
        QString clearErr;
        const int clearEc = clearBrokenMountEntry(ident.mountpoint, &clearErr);
        if (clearEc != 0 || !waitForMountToDisappear(ident.mountpoint, kUnmountDisappearTimeoutMs)) {
            QJsonObject details;
            details.insert(QStringLiteral("mountpoint"), ident.mountpoint);
            details.insert(QStringLiteral("mount_health"), mountLivenessName(liveness));
            details.insert(QStringLiteral("clear_exit_code"), clearEc);
            if (!clearErr.isEmpty())
                details.insert(QStringLiteral("_stderr"), clearErr);

            QString msg = (liveness == MountLiveness::Unresponsive)
                ? QStringLiteral("a mount at %1 is not responding and could not be cleared; "
                                 "its daemon is still holding it, so mounting over it cannot succeed")
                      .arg(ident.mountpoint)
                : QStringLiteral("a stale mount at %1 could not be cleared; "
                                 "its daemon is gone but the entry remains")
                      .arg(ident.mountpoint);
            if (!clearErr.trimmed().isEmpty())
                msg += QStringLiteral("; stderr: %1").arg(clearErr.trimmed());

            auto env = errEnvelope(QStringLiteral("mount"), imagePath, 6, QStringLiteral("MountStateError"), msg);
            QJsonObject root = env;
            root.insert(QStringLiteral("details"), details);
            return QJsonDocument(root).toJson(QJsonDocument::Indented);
        }

        // The leftover is gone; drop its registry record so the mount below
        // re-registers it cleanly.
        const auto regP = registryPaths();
        QLockFile dropLock(regP.lock);
        dropLock.setStaleLockTime(0);
        dropLock.tryLock(5000);
        auto dropObj = loadRegistryUnlocked(regP);
        const auto dropMounts = dropObj.value(QStringLiteral("mounts")).toArray();
        QJsonArray kept;
        for (const auto& v : dropMounts) {
            const auto rec = v.toObject();
            if (rec.value(QStringLiteral("mountpoint")).toString() == ident.mountpoint
                || rec.value(QStringLiteral("image")).toString() == ident.canonicalImage)
                continue;
            kept.append(rec);
        }
        dropObj.insert(QStringLiteral("mounts"), kept);
        QString dropErr;
        saveRegistryUnlocked(regP, dropObj, &dropErr);
    }

    const auto engineOverride = qEnvironmentVariable("DWARFSCTL_ENGINE");
    const auto dwarfs = engineOverride.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../libexec/dwarfsctl-engine"))
        : QFileInfo(engineOverride).absoluteFilePath();
    if (!QFileInfo(dwarfs).isExecutable()) {
        const auto env = errEnvelope(QStringLiteral("mount"), imagePath, 3, QStringLiteral("DependencyMissing"), QStringLiteral("bundled dwarfsctl engine is unavailable"));
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }

    int ec = 0;
    QString se;
    const QStringList mountArgs = {QStringLiteral("dwarfs"),
                                   ident.canonicalImage,
                                   ident.mountpoint,
                                   QStringLiteral("-o"),
                                   QStringLiteral("ro"),
                                   QStringLiteral("-o"),
                                   QStringLiteral("readonly")};
    const QByteArray soBytes = runTool(dwarfs, mountArgs, kToolTimeoutMs, &ec, &se);
    const QString so = QString::fromUtf8(soBytes);

    if (ec != 0) {
        QJsonObject details;
        details.insert(QStringLiteral("program"), dwarfs);
        details.insert(QStringLiteral("args"), QJsonArray::fromStringList(mountArgs));
        details.insert(QStringLiteral("exit_code"), ec);
        details.insert(QStringLiteral("_stdout"), so);
        details.insert(QStringLiteral("_stderr"), se);
        details.insert(QStringLiteral("mountpoint"), ident.mountpoint);

        auto env = errEnvelope(QStringLiteral("mount"), imagePath, 1, QStringLiteral("MountState"),
                               se.trimmed().isEmpty() ? QStringLiteral("The image could not be mounted (code %1)").arg(ec) : se.trimmed().right(2000));
        QJsonObject root = env;
        root.insert(QStringLiteral("details"), details);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    // Wait for mount to appear.
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kMountAppearTimeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (isMounted(ident.mountpoint))
            break;
        QThread::msleep(50);
    }

    if (!isMounted(ident.mountpoint)) {
        QJsonObject details;
        details.insert(QStringLiteral("program"), dwarfs);
        details.insert(QStringLiteral("args"), QJsonArray::fromStringList(mountArgs));
        details.insert(QStringLiteral("exit_code"), ec);
        details.insert(QStringLiteral("_stdout"), so);
        details.insert(QStringLiteral("_stderr"), se);
        details.insert(QStringLiteral("mountpoint"), ident.mountpoint);

        QString msg = QStringLiteral("mount did not appear within %1ms").arg(kMountAppearTimeoutMs);
        if (!se.trimmed().isEmpty())
            msg += QStringLiteral("; stderr: %1").arg(se.trimmed());
        if (!so.trimmed().isEmpty())
            msg += QStringLiteral("; stdout: %1").arg(so.trimmed());

        auto env = errEnvelope(QStringLiteral("mount"), imagePath, 1, QStringLiteral("MountState"), msg);
        QJsonObject root = env;
        root.insert(QStringLiteral("details"), details);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }

    // Update registry.
    const auto p = registryPaths();
    QLockFile regLock(p.lock);
    regLock.setStaleLockTime(0);
    regLock.tryLock(5000);
    auto obj = loadRegistryUnlocked(p);
    auto mounts = obj.value(QStringLiteral("mounts")).toArray();
    bool updated = false;
    for (int i = 0; i < mounts.size(); ++i) {
        const auto rec = mounts.at(i).toObject();
        if (rec.value(QStringLiteral("image")).toString() == ident.canonicalImage || rec.value(QStringLiteral("mountpoint")).toString() == ident.mountpoint) {
            QJsonObject n;
            n.insert(QStringLiteral("image"), ident.canonicalImage);
            n.insert(QStringLiteral("mountpoint"), ident.mountpoint);
            n.insert(QStringLiteral("mounted_at"), QDateTime::currentSecsSinceEpoch());
            n.insert(QStringLiteral("pid"), static_cast<int>(getpid()));
            mounts.replace(i, n);
            updated = true;
            break;
        }
    }
    if (!updated) {
        QJsonObject n;
        n.insert(QStringLiteral("image"), ident.canonicalImage);
        n.insert(QStringLiteral("mountpoint"), ident.mountpoint);
        n.insert(QStringLiteral("mounted_at"), QDateTime::currentSecsSinceEpoch());
        n.insert(QStringLiteral("pid"), static_cast<int>(getpid()));
        mounts.append(n);
    }
    obj.insert(QStringLiteral("mounts"), mounts);
    QString saveErr;
    if (!saveRegistryUnlocked(p, obj, &saveErr)) {
        // Ignore; mount worked.
    }

    // Record successful mounts for the tray UX.
    dwarfsctl_tray::recordRecentlyMountedImage(ident.canonicalImage);

    QJsonObject details;
    details.insert(QStringLiteral("mounted"), true);
    details.insert(QStringLiteral("mountpoint"), ident.mountpoint);
    const auto env = okEnvelope(QStringLiteral("mount"), imagePath, details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

static QByteArray doUnmountInternal(const QString& command, const QString& imagePath, const QString& mountpoint)
{
    QString tool = resolveTool(QStringLiteral("fusermount3"));
    QStringList args;
    if (!tool.isEmpty()) {
        args = {QStringLiteral("-u"), mountpoint};
    } else {
        tool = resolveTool(QStringLiteral("fusermount"));
        if (!tool.isEmpty()) {
            args = {QStringLiteral("-u"), mountpoint};
        } else {
            tool = resolveTool(QStringLiteral("umount"));
            if (!tool.isEmpty()) {
                args = {mountpoint};
            }
        }
    }
    if (tool.isEmpty()) {
        const auto env = errEnvelope(command, imagePath, 1, QStringLiteral("Cli"), QStringLiteral("no unmount tool found (fusermount3/fusermount/umount)"));
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }

    int ec = 0;
    QString se;
    runTool(tool, args, kToolTimeoutMs, &ec, &se);

    // Wait briefly for mount to disappear.
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kUnmountDisappearTimeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (!isMounted(mountpoint))
            break;
        QThread::msleep(50);
    }

    if (isMounted(mountpoint)) {
        const auto env = errEnvelope(command, imagePath, 1, QStringLiteral("MountState"), QStringLiteral("unmount did not complete; stderr: %1").arg(se.trimmed()));
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }

    // Drop registry keys.
    const auto p = registryPaths();
    QLockFile regLock(p.lock);
    regLock.setStaleLockTime(0);
    regLock.tryLock(5000);
    auto obj = loadRegistryUnlocked(p);
    auto mounts = obj.value(QStringLiteral("mounts")).toArray();
    QJsonArray out;
    for (const auto& v : mounts) {
        const auto rec = v.toObject();
        const auto img = rec.value(QStringLiteral("image")).toString();
        const auto mp = rec.value(QStringLiteral("mountpoint")).toString();
        if (img == imagePath || mp == mountpoint)
            continue;
        out.append(rec);
    }
    obj.insert(QStringLiteral("mounts"), out);
    QString saveErr;
    saveRegistryUnlocked(p, obj, &saveErr);
    removeManagedMountDirectory(mountpoint);

    QJsonObject details;
    details.insert(QStringLiteral("mounted"), false);
    details.insert(QStringLiteral("mountpoint"), mountpoint);
    const auto env = okEnvelope(command, imagePath, details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

QByteArray MountExecutor::unmountByMountpoint(const QString& mountpoint)
{
    return doUnmountInternal(QStringLiteral("unmount"), QString(), mountpoint);
}

QByteArray MountExecutor::unmountByImage(const QString& imagePath)
{
    const auto ident = resolveIdentity(imagePath);
    return doUnmountInternal(QStringLiteral("unmount"), ident.canonicalImage.isEmpty() ? imagePath : ident.canonicalImage, ident.mountpoint);
}

QByteArray MountExecutor::unmountAll()
{
    // Iterate based on the on-disk registry (avoids stale cache windows).
    const auto p = registryPaths();
    const auto reg = loadRegistryUnlocked(p);
    QJsonArray arr = reg.value(QStringLiteral("mounts")).toArray();
    if (arr.isEmpty()) {
        const auto env = okEnvelope(QStringLiteral("unmount_all"), QString(), QJsonObject{{QStringLiteral("count"), 0}});
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }
    QJsonArray results;
    QJsonArray failed;
    for (const auto& v : arr) {
        const auto o = v.toObject();
        const auto img = o.value(QStringLiteral("image")).toString();
        const auto mp = o.value(QStringLiteral("mountpoint")).toString();
        const auto out = QJsonDocument::fromJson(doUnmountInternal(QStringLiteral("unmount"), img, mp)).object();
        results.append(out);
        if (!out.value(QStringLiteral("ok")).toBool())
            failed.append(img);
    }
    QJsonObject details;
    details.insert(QStringLiteral("requested"), arr.size());
    details.insert(QStringLiteral("results"), results);
    details.insert(QStringLiteral("failed"), failed);
    if (!failed.isEmpty()) {
        const auto env = errEnvelope(QStringLiteral("unmount_all"), QString(), 1, QStringLiteral("MountState"), QStringLiteral("failed to unmount one or more mount(s)"));
        QJsonObject root = env;
        root.insert(QStringLiteral("details"), details);
        return QJsonDocument(root).toJson(QJsonDocument::Indented);
    }
    const auto env = okEnvelope(QStringLiteral("unmount_all"), QString(), details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

QByteArray MountExecutor::unmountLast()
{
    // Find max mounted_at from registry file.
    const auto p = registryPaths();
    QLockFile regLock(p.lock);
    regLock.setStaleLockTime(0);
    regLock.tryLock(5000);
    const auto obj = loadRegistryUnlocked(p);
    const auto mounts = obj.value(QStringLiteral("mounts")).toArray();
    if (mounts.isEmpty()) {
        const auto env = okEnvelope(QStringLiteral("unmount_last"), QString(), QJsonObject{{QStringLiteral("count"), 0}});
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }
    QJsonObject best;
    qint64 bestTs = -1;
    for (const auto& v : mounts) {
        const auto rec = v.toObject();
        const auto ts = static_cast<qint64>(rec.value(QStringLiteral("mounted_at")).toDouble());
        if (ts > bestTs) {
            bestTs = ts;
            best = rec;
        }
    }
    return doUnmountInternal(QStringLiteral("unmount_last"), best.value(QStringLiteral("image")).toString(), best.value(QStringLiteral("mountpoint")).toString());
}
