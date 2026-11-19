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
#include <QUuid>
#include <QHash>

#include <csignal>
#include <stdexcept>
#include <sys/prctl.h>
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

class RegistryError final : public std::runtime_error {
public:
    explicit RegistryError(const QString &message, const QString &category = QStringLiteral("Registry"),
                           int status = 1, const QJsonObject &state = {})
        : std::runtime_error(message.toStdString()), kind(category), code(status), details(state) {}
    QString kind;
    int code;
    QJsonObject details;
};

QByteArray registryError(const QString &command, const QString &image, const RegistryError &error)
{
    auto envelope = errEnvelope(command, image, error.code, error.kind, QString::fromUtf8(error.what()));
    if (!error.details.isEmpty()) envelope.insert(QStringLiteral("details"), error.details);
    return QJsonDocument(envelope).toJson(QJsonDocument::Indented);
}

MountObservation checkedObservation(const QString &point)
{
    const auto observed = observeMount(point);
    if (observed.presence == MountPresence::Unknown)
        throw RegistryError(QStringLiteral("The kernel mount state could not be checked. Recorded volumes were preserved; try again when the mount state is available."),
            QStringLiteral("MountStateError"), 6, {{QStringLiteral("mount_state_known"), false},
                {QStringLiteral("reason"), QStringLiteral("kernel_observation_unavailable")}, {QStringLiteral("mountpoint"), point}});
    return observed;
}

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
    const QFileInfo info(p.registry);
    if (info.isSymLink())
        throw RegistryError(QStringLiteral("The mount registry must be a regular file."));
    if (!info.exists()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("mounts"), QJsonArray{});
        return obj;
    }
    if (!info.isFile() || !f.open(QIODevice::ReadOnly))
        throw RegistryError(QStringLiteral("The mount registry could not be read: %1").arg(f.errorString()));
    QJsonParseError parseError;
    const auto bytes = f.readAll();
    if (f.error() != QFile::NoError)
        throw RegistryError(QStringLiteral("The mount registry could not be read: %1").arg(f.errorString()));
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        throw RegistryError(QStringLiteral("The mount registry is invalid; its contents were preserved."));
    auto obj = doc.object();
    if (!obj.value(QStringLiteral("mounts")).isArray())
        throw RegistryError(QStringLiteral("The mount registry has an invalid mount list; its contents were preserved."));
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
    const auto data = doc.toJson(QJsonDocument::Indented) + '\n';
    if (sf.write(data) != data.size()) {
        if (err) *err = QStringLiteral("failed to write mount registry: %1").arg(sf.errorString());
        sf.cancelWriting();
        return false;
    }
    if (!sf.commit()) {
        if (err)
            *err = QStringLiteral("failed to commit registry: %1").arg(sf.errorString());
        return false;
    }
    return true;
}

class Registry final {
public:
    Registry() : paths(registryPaths()), lock(paths.lock)
    {
        if (!QDir().mkpath(paths.dir))
            throw RegistryError(QStringLiteral("The mount registry directory could not be created."));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(5000))
            throw RegistryError(QStringLiteral("The mount registry is busy or unavailable (%1).")
                .arg(lockErrorToString(lock.error())));
        value = loadRegistryUnlocked(paths);
    }
    void commit()
    {
        QString message;
        if (!saveRegistryUnlocked(paths, value, &message)) throw RegistryError(message);
    }
    RegistryPaths paths;
    QLockFile lock;
    QJsonObject value;
};

QJsonObject sourceGeneration(const QString &image, bool *known = nullptr)
{
    struct stat value {};
    int error = 0;
    const bool available = boundedFileStat(image, &value, &error);
    if (known) *known = available || error == ENOENT || error == ENOTDIR;
    if (!available || !S_ISREG(value.st_mode)) return {};
    return {{QStringLiteral("device"), QString::number(value.st_dev)},
        {QStringLiteral("inode"), QString::number(value.st_ino)},
        {QStringLiteral("size"), QString::number(value.st_size)},
        {QStringLiteral("mtime_seconds"), QString::number(value.st_mtim.tv_sec)},
        {QStringLiteral("mtime_nanoseconds"), QString::number(value.st_mtim.tv_nsec)},
        {QStringLiteral("ctime_seconds"), QString::number(value.st_ctim.tv_sec)},
        {QStringLiteral("ctime_nanoseconds"), QString::number(value.st_ctim.tv_nsec)}};
}

QJsonObject mountRecord(const QString &image, const QString &mountpoint,
                       const QString &operation = {}, const QJsonObject &generation = {})
{
    QJsonObject record{{QStringLiteral("image"), image}, {QStringLiteral("mountpoint"), mountpoint},
        {QStringLiteral("mounted_at"), QDateTime::currentSecsSinceEpoch()},
        {QStringLiteral("pid"), static_cast<int>(getpid())}};
    if (!generation.isEmpty()) record.insert(QStringLiteral("source_generation"), generation);
    if (!operation.isEmpty()) {
        record.insert(QStringLiteral("state"), QStringLiteral("pending"));
        record.insert(QStringLiteral("operation"), operation);
    }
    return record;
}

void storeMount(const QJsonObject &record)
{
    Registry registry;
    auto mounts = registry.value.value(QStringLiteral("mounts")).toArray();
    QJsonArray updated;
    bool replaced = false;
    for (const auto &entry : mounts) {
        const auto old = entry.toObject();
        if (old.value(QStringLiteral("image")) == record.value(QStringLiteral("image"))
            || old.value(QStringLiteral("mountpoint")) == record.value(QStringLiteral("mountpoint"))) {
            if (!replaced) updated.append(record);
            replaced = true;
        } else updated.append(entry);
    }
    if (!replaced) updated.append(record);
    registry.value.insert(QStringLiteral("mounts"), updated);
    registry.commit();
}

void removeFailedIntent(const QString &operation, const QString &mountpoint)
{
    // A launcher can fail after the kernel has mounted or after a daemon fork.
    // Never erase a kernel-backed intent merely because the launcher failed.
    if (observeMount(mountpoint).presence != MountPresence::Absent) return;
    Registry registry;
    QJsonArray kept;
    for (const auto &entry : registry.value.value(QStringLiteral("mounts")).toArray()) {
        if (entry.toObject().value(QStringLiteral("operation")).toString() != operation)
            kept.append(entry);
    }
    registry.value.insert(QStringLiteral("mounts"), kept);
    registry.commit();
}

QByteArray runTool(const QString& program, const QStringList& args, int timeoutMs, int* exitCode,
                   QString* stdErr, bool *normalCompletion = nullptr)
{
    if (normalCompletion) *normalCompletion = false;
    QProcess p;
    const pid_t owner = ::getpid();
    p.setChildProcessModifier([owner] {
        // This transient launcher must not run on after its authority dies.
        // Linux clears the setting in forked children, so an accepted FUSE
        // daemon remains independent after its normal daemonization handshake.
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != owner) ::_exit(126);
    });
    p.setProgram(program);
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start();
    if (!p.waitForStarted(3000)) {
        if (normalCompletion) *normalCompletion = true; // No tool was started.
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
    if (normalCompletion) *normalCompletion = p.exitStatus() == QProcess::NormalExit;
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
        if (checkedObservation(mountpoint).presence == MountPresence::Absent)
            return true;
        QThread::msleep(50);
    }
    return checkedObservation(mountpoint).presence == MountPresence::Absent;
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
    const auto canonical = boundedCanonicalFilePath(imagePath);
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
    // Keep the component comfortably below NAME_MAX even for Unicode names.
    while (safe.toUtf8().size() > 48) safe.chop(1);
    const auto mountDir = safe + QStringLiteral("-") + QString::fromLatin1(hash);
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
    if (info.dir().absolutePath() != root || observeMount(path).presence != MountPresence::Absent) {
        return;
    }
    QDir(root).rmdir(info.fileName());
}

QJsonArray usableRegistryMounts(bool removeDirectories, bool &cleaned,
                               QHash<QString, MountLiveness> *health = nullptr)
{
    QJsonArray snapshot;
    { Registry registry; snapshot = registry.value.value(QStringLiteral("mounts")).toArray(); }
    QJsonArray visible;
    cleaned = false;
    for (const auto &entry : snapshot) {
        if (!entry.isObject()) continue;
        auto record = entry.toObject();
        const auto source = record.value(QStringLiteral("image")).toString();
        const auto point = record.value(QStringLiteral("mountpoint")).toString();
        if (source.isEmpty() || point.isEmpty()) continue;
        const auto directory = xdgRuntimeDir() + QStringLiteral("/dwarfs/locks");
        if (!QDir().mkpath(directory)) throw RegistryError(QStringLiteral("The mount operation lock directory could not be created."));
        const auto key = QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex();
        QLockFile authority(directory + QLatin1Char('/') + QString::fromLatin1(key) + QStringLiteral(".lock"));
        authority.setStaleLockTime(60 * 1000);
        if (!authority.tryLock(0)) throw RegistryError(QStringLiteral("Another operation is using this image. Refresh the mount state when it finishes."));
        {
            Registry registry;
            if (!registry.value.value(QStringLiteral("mounts")).toArray().contains(entry)) continue;
        }
        // Keep the registry available while a filesystem or its daemon stalls.
        // The image lock protects launch/unmount authority throughout this row.
        const auto observed = checkedObservation(point);
        const bool listed = observed.presence == MountPresence::Present;
        const bool matches = listed && QDir::cleanPath(observed.source) == QDir::cleanPath(source);
        const auto probed = matches ? probeMountLiveness(point) : MountLiveness::Unknown;
        const bool pending = record.value(QStringLiteral("state")).toString() == QLatin1String("pending");
        if (pending && matches && probed == MountLiveness::Unknown)
            throw RegistryError(QStringLiteral("A recorded mount is still pending and its health could not be checked. Its recovery information was preserved."),
                QStringLiteral("MountStateError"), 6, {{QStringLiteral("mount_state_known"), false},
                    {QStringLiteral("reason"), QStringLiteral("pending_health_unknown")}, {QStringLiteral("mountpoint"), point}});
        const auto currentObservation = checkedObservation(point);
        const bool usable = matches && mountEntryIsUsable(listed, probed)
            && (!pending || probed == MountLiveness::Live)
            && currentObservation.presence == MountPresence::Present
            && QDir::cleanPath(currentObservation.source) == QDir::cleanPath(source);
        const bool remove = !listed && !pending;
        auto updated = record;
        if (usable && pending) {
            updated.remove(QStringLiteral("state"));
            updated.remove(QStringLiteral("operation"));
            updated.insert(QStringLiteral("pid"), static_cast<int>(getpid()));
        }
        bool accepted = false;
        {
            Registry registry;
            const auto current = registry.value.value(QStringLiteral("mounts")).toArray();
            QJsonArray kept;
            for (const auto &row : current) {
                if (row != entry) { kept.append(row); continue; }
                accepted = true;
                if (!remove) kept.append(updated);
            }
            if (accepted && (remove || updated != record)) {
                registry.value.insert(QStringLiteral("mounts"), kept);
                registry.commit();
            }
        }
        if (!accepted) continue;
        if (usable) {
            visible.append(updated);
            if (health) health->insert(point, probed);
        } else if (remove) {
            cleaned = true;
            if (removeDirectories) removeManagedMountDirectory(point);
        }
    }
    return visible;
}

} // namespace

QByteArray MountExecutor::listMounts() try
{
    bool cleaned = false;
    const auto filtered = usableRegistryMounts(true, cleaned);

    QJsonObject details;
    details.insert(QStringLiteral("count"), filtered.size());
    details.insert(QStringLiteral("cleaned_stale"), cleaned);
    details.insert(QStringLiteral("mounts"), filtered);

    const auto env = okEnvelope(QStringLiteral("mount_list"), QString(), details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("mount_list"), {}, error);
}

QByteArray MountExecutor::status(const QString& imagePath) try
{
    bool cleaned = false;
    QHash<QString, MountLiveness> health;
    const auto filtered = usableRegistryMounts(false, cleaned, &health);

    const QFileInfo fi(imagePath);
    const bool resolveBeforeMatch = imagePath.split(QLatin1Char('/')).contains(QStringLiteral(".."));
    QString canonical;
    QString foundMp;
    QJsonObject foundRecord;
    for (int attempt = 0; attempt < 2 && foundMp.isEmpty(); ++attempt) {
      if (attempt == 1) canonical = boundedCanonicalFilePath(imagePath);
      for (const auto& v : filtered) {
        if (!v.isObject())
            continue;
        const auto o = v.toObject();
        const auto img = o.value(QStringLiteral("image")).toString();
        const auto point = o.value(QStringLiteral("mountpoint")).toString();
        const auto selector = QDir::cleanPath(fi.absoluteFilePath());
        if (img == imagePath || (!resolveBeforeMatch && point == selector) || (!canonical.isEmpty() && (img == canonical || point == canonical))) {
            foundMp = o.value(QStringLiteral("mountpoint")).toString();
            foundRecord = o;
            break;
        }
    }

    }
    // Reuse the snapshot's single health observation; a second probe would
    // stretch response time and could disagree with the observation just made.
    const bool mounted = !foundMp.isEmpty();

    QJsonObject details;
    details.insert(QStringLiteral("target"), imagePath);
    details.insert(QStringLiteral("found"), foundMp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(foundMp));
    details.insert(QStringLiteral("mounted"), mounted);
    details.insert(QStringLiteral("mountpoint"), foundMp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(foundMp));
    if (!foundMp.isEmpty()) {
        bool sourceKnown = false;
        const auto generation = sourceGeneration(foundRecord.value(QStringLiteral("image")).toString(), &sourceKnown);
        const auto saved = foundRecord.value(QStringLiteral("source_generation")).toObject();
        details.insert(QStringLiteral("source_available"), sourceKnown ? QJsonValue(!generation.isEmpty()) : QJsonValue(QJsonValue::Null));
        details.insert(QStringLiteral("generation_verified"), sourceKnown ? QJsonValue(!saved.isEmpty() && saved == generation) : QJsonValue(QJsonValue::Null));
        details.insert(QStringLiteral("source_changed"), sourceKnown ? QJsonValue(!saved.isEmpty() && !generation.isEmpty() && saved != generation) : QJsonValue(QJsonValue::Null));
    }
    if (!foundMp.isEmpty())
        details.insert(QStringLiteral("mount_health"), mountLivenessName(health.value(foundMp, MountLiveness::Unknown)));

    const auto env = okEnvelope(QStringLiteral("mount_status"), imagePath, details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("mount_status"), imagePath, error);
}

QByteArray MountExecutor::mount(const QString& imagePath) try
{
    auto ident = resolveIdentity(imagePath);
    if (ident.canonicalImage.isEmpty()) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 1,
            QStringLiteral("Cli"), QStringLiteral("The image path could not be resolved. It may be unavailable or temporarily unresponsive."))).toJson(QJsonDocument::Indented);
    }
    QDir().mkpath(ident.mountRoot);
    QDir().mkpath(ident.lockDir);
    QLockFile lock(ident.lockPath);
    lock.setStaleLockTime(60 * 1000);
    if (!lock.tryLock(5000)) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 1,
            QStringLiteral("MountState"), QStringLiteral("failed to acquire mount lock (%1)")
                .arg(lockErrorToString(lock.error())))).toJson(QJsonDocument::Indented);
    }

    QJsonObject previous;
    {
        Registry registry;
        for (const auto &value : registry.value.value(QStringLiteral("mounts")).toArray()) {
            const auto record = value.toObject();
            if (record.value(QStringLiteral("image")).toString() != ident.canonicalImage) continue;
            const auto point = QDir::cleanPath(record.value(QStringLiteral("mountpoint")).toString());
            // Preserve recorded paths from earlier versions, but only within
            // our managed root; never derive a replacement for a live volume.
            if (QFileInfo(point).dir().absolutePath() == QDir::cleanPath(ident.mountRoot)) {
                previous = record;
                ident.mountpoint = point;
                break;
            }
        }
    }
    const auto generation = sourceGeneration(ident.canonicalImage);
    if (generation.isEmpty()) return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 1,
        QStringLiteral("MountState"), QStringLiteral("The image is not an available regular file."))).toJson();
    QDir().mkpath(ident.mountpoint);

    const auto observed = checkedObservation(ident.mountpoint);
    const bool listed = observed.presence == MountPresence::Present;
    const auto health = listed ? probeMountLiveness(ident.mountpoint) : MountLiveness::Unknown;
    if (listed && QDir::cleanPath(observed.source) != QDir::cleanPath(ident.canonicalImage)) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 6,
            QStringLiteral("MountStateError"), QStringLiteral("This mountpoint belongs to a different source. Unmount it before trying again."))).toJson();
    }
    if (listed && health == MountLiveness::Unknown) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 6,
            QStringLiteral("MountStateError"), QStringLiteral("The existing volume's health could not be checked. Try again after other mount operations finish."))).toJson();
    }
    if (listed && health == MountLiveness::Live) {
        const auto saved = previous.value(QStringLiteral("source_generation")).toObject();
        if (saved.isEmpty() || saved != generation) {
            auto response = errEnvelope(QStringLiteral("mount"), imagePath, 6,
                QStringLiteral("MountStateError"), saved.isEmpty()
                    ? QStringLiteral("This existing volume's source version cannot be verified. Unmount it, then mount the image again.")
                    : QStringLiteral("The image has changed since this volume was mounted. Unmount it, then mount the image again."));
            response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("mounted"), true},
                {QStringLiteral("mountpoint"), ident.mountpoint}, {QStringLiteral("generation_verified"), false}});
            return QJsonDocument(response).toJson();
        }
        if (!mountImageMatches(ident.mountpoint, ident.canonicalImage) || sourceGeneration(ident.canonicalImage) != generation) {
            return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 6,
                QStringLiteral("MountStateError"), QStringLiteral("The volume or its source changed. Check the mount state before trying again.")))
                .toJson(QJsonDocument::Indented);
        }
        (void)dwarfsctl_kde::rememberImage(ident.canonicalImage, [](const QString &path) {
            const auto canonical = boundedCanonicalFilePath(path);
            return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
        });
        return QJsonDocument(okEnvelope(QStringLiteral("mount"), imagePath,
            QJsonObject{{QStringLiteral("mounted"), true}, {QStringLiteral("mountpoint"), ident.mountpoint},
                {QStringLiteral("mount_health"), mountLivenessName(health)}})).toJson(QJsonDocument::Indented);
    }

    const auto engineOverride = qEnvironmentVariable("DWARFSCTL_ENGINE");
    const QString engine = engineOverride.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../libexec/dwarfsctl-engine"))
        : QFileInfo(engineOverride).absoluteFilePath();
    if (!QFileInfo(engine).isExecutable()) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 3,
            QStringLiteral("DependencyMissing"), QStringLiteral("bundled dwarfsctl engine is unavailable")))
            .toJson(QJsonDocument::Indented);
    }

    // Commit recoverable intent before any launcher or stale-mount cleanup can
    // change the kernel. A restart can adopt it only if this exact managed
    // mountpoint really exists and serves requests; intent alone is not success.
    const QString operation = QUuid::createUuid().toString(QUuid::WithoutBraces);
    storeMount(mountRecord(ident.canonicalImage, ident.mountpoint, operation, generation));

    if (listed) {
        QString message;
        const int code = clearBrokenMountEntry(ident.mountpoint, &message);
        if (code != 0 || !waitForMountToDisappear(ident.mountpoint, kUnmountDisappearTimeoutMs)) {
            auto response = errEnvelope(QStringLiteral("mount"), imagePath, 6,
                QStringLiteral("MountStateError"), QStringLiteral("The previous mount could not be cleared. %1").arg(message));
            response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("mountpoint"), ident.mountpoint},
                {QStringLiteral("mount_health"), mountLivenessName(health)}, {QStringLiteral("clear_exit_code"), code}});
            return QJsonDocument(response).toJson(QJsonDocument::Indented);
        }
    }

    const QStringList args{QStringLiteral("dwarfs"), ident.canonicalImage, ident.mountpoint,
        QStringLiteral("-o"), QStringLiteral("ro"), QStringLiteral("-o"), QStringLiteral("readonly")};
    int code = 0;
    QString stderrText;
    bool completedNormally = false;
    const auto stdoutText = runTool(engine, args, kToolTimeoutMs, &code, &stderrText, &completedNormally);
    if (code != 0) {
        // Pinned DwarFS mounts before daemonization. A normal failed return with
        // no kernel mount is safe to discard. A crash/timeout has an unknown
        // handoff outcome, so preserve its intent for a later kernel check.
        if (completedNormally) removeFailedIntent(operation, ident.mountpoint);
        auto response = errEnvelope(QStringLiteral("mount"), imagePath, 1, QStringLiteral("MountState"),
            stderrText.trimmed().isEmpty() ? QStringLiteral("The image could not be mounted (code %1)").arg(code)
                                         : stderrText.trimmed().right(2000));
        response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("program"), engine},
            {QStringLiteral("args"), QJsonArray::fromStringList(args)}, {QStringLiteral("exit_code"), code},
            {QStringLiteral("_stdout"), QString::fromUtf8(stdoutText)}, {QStringLiteral("_stderr"), stderrText},
            {QStringLiteral("mountpoint"), ident.mountpoint}});
        return QJsonDocument(response).toJson(QJsonDocument::Indented);
    }

    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + kMountAppearTimeoutMs;
    while (checkedObservation(ident.mountpoint).presence == MountPresence::Absent && QDateTime::currentMSecsSinceEpoch() < deadline)
        QThread::msleep(50);
    if (!mountImageMatches(ident.mountpoint, ident.canonicalImage) || probeMountLiveness(ident.mountpoint) != MountLiveness::Live) {
        // A successful launcher reply is not a successful kernel volume.
        // Preserve unknown intent rather than lose a possible delayed handoff.
        auto response = errEnvelope(QStringLiteral("mount"), imagePath, 6, QStringLiteral("MountStateError"),
            QStringLiteral("The mount did not become available. Check the current mount state before trying again."));
        response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("mountpoint"), ident.mountpoint},
            {QStringLiteral("exit_code"), code}});
        return QJsonDocument(response).toJson(QJsonDocument::Indented);
    }

    // Atomic finalization preserves the pending row if persistence fails. Do
    // not claim completion until both this commit and a fresh kernel check pass.
    if (sourceGeneration(ident.canonicalImage) != generation) {
        // Keep the kernel-backed intent controllable. Do not silently retarget
        // or unmount a volume when the source changed during the launch.
        auto response = errEnvelope(QStringLiteral("mount"), imagePath, 6, QStringLiteral("MountStateError"),
            QStringLiteral("The image changed while mounting. Unmount this volume before mounting the image again."));
        response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("mounted"), true},
            {QStringLiteral("mountpoint"), ident.mountpoint}, {QStringLiteral("generation_verified"), false}});
        return QJsonDocument(response).toJson();
    }
    storeMount(mountRecord(ident.canonicalImage, ident.mountpoint, {}, generation));
    if (!mountImageMatches(ident.mountpoint, ident.canonicalImage) || sourceGeneration(ident.canonicalImage) != generation) {
        return QJsonDocument(errEnvelope(QStringLiteral("mount"), imagePath, 6, QStringLiteral("MountStateError"),
            QStringLiteral("The mounted volume is no longer available."))).toJson(QJsonDocument::Indented);
    }
    (void)dwarfsctl_kde::rememberImage(ident.canonicalImage, [](const QString &path) {
            const auto canonical = boundedCanonicalFilePath(path);
            return canonical.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : canonical;
        });
    return QJsonDocument(okEnvelope(QStringLiteral("mount"), imagePath,
        QJsonObject{{QStringLiteral("mounted"), true}, {QStringLiteral("mountpoint"), ident.mountpoint}}))
        .toJson(QJsonDocument::Indented);
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("mount"), imagePath, error);
}

static QByteArray doUnmountInternal(const QString& command, const QString& imagePath, const QString& mountpoint) try
{
    // Mount and unmount share a source lock across callers/processes. In
    // particular, an unmount must not erase a pending launch before it mounts.
    QString source = imagePath;
    bool recordedSource = false;
    QJsonObject selectedRecord;
    {
        Registry preflight;
        for (const auto &entry : preflight.value.value(QStringLiteral("mounts")).toArray()) {
            const auto record = entry.toObject();
            if (record.value(QStringLiteral("mountpoint")).toString() == mountpoint) {
                source = record.value(QStringLiteral("image")).toString();
                recordedSource = true;
                selectedRecord = record;
                break;
            }
        }
    }
    if (source.isEmpty()) {
        // Without a recorded image there is no shared launch authority. In
        // particular, do not unmount a path while its first intent is arriving.
        auto response = errEnvelope(command, imagePath, 1, QStringLiteral("MountState"),
            QStringLiteral("No recorded volume was found at this mountpoint. Refresh the mount list before trying again."));
        response.insert(QStringLiteral("details"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("record_not_found")}});
        return QJsonDocument(response).toJson();
    }
    const QString canonical = recordedSource ? QString{} : boundedCanonicalFilePath(source);
    if (!recordedSource && !canonical.isEmpty()) source = canonical;
    const QString key = source.isEmpty() ? QDir::cleanPath(mountpoint) : source;
    const auto lockDirectory = xdgRuntimeDir() + QStringLiteral("/dwarfs/locks");
    if (!QDir().mkpath(lockDirectory)) throw RegistryError(QStringLiteral("The mount operation lock directory could not be created."));
    const auto hash = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    QLockFile operationLock(QDir(lockDirectory).filePath(QString::fromLatin1(hash) + QStringLiteral(".lock")));
    operationLock.setStaleLockTime(60 * 1000);
    if (!operationLock.tryLock(5000))
        return QJsonDocument(errEnvelope(command, imagePath, 1, QStringLiteral("Busy"),
            QStringLiteral("Another operation is using this image. Try again when it finishes."))).toJson();
    // The row that selected this source must still own the point after the
    // shared launch lock is acquired. A registry row alone never authorizes
    // detaching an unrelated replacement filesystem.
    bool sameRecord = false;
    {
        Registry preflight;
        for (const auto &entry : preflight.value.value(QStringLiteral("mounts")).toArray())
            if (entry.toObject() == selectedRecord && recordedSource) sameRecord = true;
    }
    const auto observed = checkedObservation(mountpoint);
    if (!sameRecord || (observed.presence == MountPresence::Present && QDir::cleanPath(observed.source) != QDir::cleanPath(source)))
        return QJsonDocument(errEnvelope(command, imagePath, 6, QStringLiteral("MountStateError"),
            QStringLiteral("The recorded volume changed or this mountpoint belongs to a different source. Refresh the mount state before trying again."))).toJson();
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

    if (!waitForMountToDisappear(mountpoint, kUnmountDisappearTimeoutMs)) {
        const auto env = errEnvelope(command, imagePath, 1, QStringLiteral("MountState"), QStringLiteral("unmount did not complete; stderr: %1").arg(se.trimmed()));
        return QJsonDocument(env).toJson(QJsonDocument::Indented);
    }

    // Drop registry keys under a short lock, then clean up outside it.
    {
        Registry registry;
        auto obj = registry.value;
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
        registry.value = obj;
        registry.commit();
    }
    removeManagedMountDirectory(mountpoint);

    QJsonObject details;
    details.insert(QStringLiteral("mounted"), false);
    details.insert(QStringLiteral("mountpoint"), mountpoint);
    const auto env = okEnvelope(command, imagePath, details);
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
} catch (const RegistryError &error) {
    return registryError(command, imagePath, error);
}

QByteArray MountExecutor::unmountByMountpoint(const QString& mountpoint) try
{
    const auto absolute = QDir::cleanPath(QFileInfo(mountpoint).absoluteFilePath());
    const bool resolveBeforeMatch = mountpoint.split(QLatin1Char('/')).contains(QStringLiteral(".."));
    QString canonical;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) canonical = boundedCanonicalFilePath(mountpoint);
        QString selected;
        {
            Registry registry;
            for (const auto &entry : registry.value.value(QStringLiteral("mounts")).toArray()) {
                const auto point = entry.toObject().value(QStringLiteral("mountpoint")).toString();
                if (point == mountpoint || (!resolveBeforeMatch && point == absolute) || (!canonical.isEmpty() && point == canonical)) {
                    selected = point;
                    break;
                }
            }
        }
        if (!selected.isEmpty()) return doUnmountInternal(QStringLiteral("unmount"), {}, selected);
    }
    return doUnmountInternal(QStringLiteral("unmount"), {}, resolveBeforeMatch ? mountpoint : absolute);
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("unmount"), {}, error);
}

QByteArray MountExecutor::unmountByImage(const QString& imagePath) try
{
    // The mounted filesystem can remain usable after its source is moved or
    // removed. Its recorded mountpoint remains authoritative for unmounting;
    // hashing an empty canonical path would target an unrelated directory.
    const QFileInfo file(imagePath);
    const QString absolute = QDir::cleanPath(file.absoluteFilePath());
    const bool resolveBeforeMatch = imagePath.split(QLatin1Char('/')).contains(QStringLiteral(".."));
    QString canonical;
    QString recorded;
    for (int attempt = 0; attempt < 2 && recorded.isEmpty(); ++attempt) {
        if (attempt == 1) canonical = boundedCanonicalFilePath(imagePath);
        Registry registry;
        for (const auto &value : registry.value.value(QStringLiteral("mounts")).toArray()) {
            const auto entry = value.toObject();
            const auto image = entry.value(QStringLiteral("image")).toString();
            if (image == imagePath || (!resolveBeforeMatch && image == absolute) || (!canonical.isEmpty() && image == canonical)) {
                recorded = entry.value(QStringLiteral("mountpoint")).toString();
                if (!recorded.isEmpty()) break;
            }
        }
    }
    if (!recorded.isEmpty()) return doUnmountInternal(QStringLiteral("unmount"), imagePath, recorded);
    if (canonical.isEmpty()) {
        return QJsonDocument(errEnvelope(QStringLiteral("unmount"), imagePath, 1,
            QStringLiteral("MountState"), QStringLiteral("No recorded mount was found and the image path could not be resolved.")))
            .toJson(QJsonDocument::Indented);
    }
    return QJsonDocument(errEnvelope(QStringLiteral("unmount"), imagePath, 1,
        QStringLiteral("MountState"), QStringLiteral("No recorded mount was found for this image. Refresh the mount list before trying again."))).toJson();
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("unmount"), imagePath, error);
}

QByteArray MountExecutor::unmountAll() try
{
    // Iterate based on the on-disk registry (avoids stale cache windows).
    QJsonArray arr;
    { Registry registry; arr = registry.value.value(QStringLiteral("mounts")).toArray(); }
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
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("unmount_all"), {}, error);
}

QByteArray MountExecutor::unmountLast() try
{
    // Find max mounted_at from registry file.
    QJsonArray mounts;
    { Registry registry; mounts = registry.value.value(QStringLiteral("mounts")).toArray(); }
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
} catch (const RegistryError &error) {
    return registryError(QStringLiteral("unmount_last"), {}, error);
}
