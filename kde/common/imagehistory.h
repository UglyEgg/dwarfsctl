// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <sys/stat.h>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <functional>
#include <memory>

namespace dwarfsctl_kde {
inline QString imageHistoryPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath(QStringLiteral("dwarfsctl/tray.ini"));
}
inline QString canonicalImagePath(const QString &path)
{
    if (path.isEmpty() || path.contains(QChar::Null)) return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}
using ImagePathResolver = std::function<QString(const QString &)>;
namespace image_history_detail {
inline constexpr auto imagesKey = "recent/images";
inline constexpr auto limitKey = "recent/limit";
inline constexpr auto migratedKey = "recent/manager-migrated-v1";
template<typename Settings> inline int limit(const Settings &settings)
{
    return qBound(0, settings.value(QLatin1String(limitKey), 5).toInt(), 9);
}
inline QString managerPath() { return QDir(QFileInfo(imageHistoryPath()).absolutePath()).filePath(QStringLiteral("manager.ini")); }
inline QStringList normalized(const QStringList &paths, int maximum, const ImagePathResolver &resolve = {})
{
    QStringList result;
    if (maximum <= 0) return result;
    for (const auto &path : paths) {
        const auto image = path.isEmpty() || path.contains(QChar::Null) ? QString{}
            : resolve ? resolve(path) : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (!image.isEmpty() && !result.contains(image, Qt::CaseSensitive)) result.append(image);
        if (result.size() >= maximum) break;
    }
    return result;
}
template<typename Settings> inline void writeImages(Settings &settings, const QStringList &images)
{
    if (images.isEmpty()) settings.remove(QLatin1String(imagesKey));
    else settings.setValue(QLatin1String(imagesKey), images);
}
inline bool sameSettingsFile(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec
        && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
// Stage edits in memory: rejecting an identity change must not leave a dirty
// QSettings whose destructor could silently commit the rejected operation.
class SettingsValues {
public:
    explicit SettingsValues(const QSettings &settings)
    {
        for (const auto &key : settings.allKeys()) m_values.insert(key, settings.value(key));
    }
    QVariant value(const QString &key, const QVariant &fallback = {}) const { return m_values.value(key, fallback); }
    void setValue(const QString &key, const QVariant &value) { m_values.insert(key, value); }
    void remove(const QString &key) { m_values.remove(key); }
    bool contains(const QString &key) const { return m_values.contains(key); }
    const QVariantMap &values() const { return m_values; }
private:
    QVariantMap m_values;
};
inline QString resolvedSettingsPath(const QString &path)
{
    const QFileInfo file(path);
    const QString existing = file.canonicalFilePath();
    if (!existing.isEmpty()) return existing;
    const QString parent = file.dir().canonicalPath();
    return parent.isEmpty() ? file.absoluteFilePath() : QDir(parent).filePath(file.fileName());
}
// QSettings caches parsed data by filename and size/mtime. A unique alias is
// needed both for reliable FormatError and for values after an atomic replace
// that preserves size/mtime. Only a symlink is written, never preference bytes.
class FreshSettings {
public:
    explicit FreshSettings(const QString &path, bool writable = false)
        : m_source(path), m_path(path)
    {
        if (!m_temporary.isValid()) return;
        m_existed = QFileInfo::exists(path) || QFileInfo(path).isSymLink();
        struct stat current {}, after {};
        if (m_existed && (!m_source.open(QIODevice::ReadOnly)
            || ::fstat(m_source.handle(), &m_identity) != 0 || !S_ISREG(m_identity.st_mode)
            || ::stat(QFile::encodeName(path).constData(), &current) != 0 || !sameSettingsFile(m_identity, current))) return;
        m_destination = resolvedSettingsPath(path);
        if (m_destination.isEmpty()) return;
        const QString alias = m_temporary.filePath(QStringLiteral("settings.ini"));
        const QString target = !writable && m_existed
            ? QStringLiteral("/proc/self/fd/%1").arg(m_source.handle()) : m_destination;
        if (!QFile::link(target, alias)) return;
        auto parsed = std::make_unique<QSettings>(alias, QSettings::IniFormat);
        parsed->setAtomicSyncRequired(true);
        parsed->allKeys();
        if (parsed->status() != QSettings::NoError) return;
        if (m_existed) {
            if (::fstat(m_source.handle(), &after) != 0 || !sameSettingsFile(m_identity, after)
                || ::stat(QFile::encodeName(path).constData(), &current) != 0 || !sameSettingsFile(m_identity, current)) return;
        } else if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) return;
        m_settings = std::move(parsed);
    }
    QSettings *get() const { return m_settings.get(); }
    bool write(const SettingsValues &values)
    {
        if (!m_settings) return false;
        if (!unchanged()) return false;
        // This is the commit boundary. The alias targets the original resolved
        // destination, so a later symlink retarget cannot redirect these writes.
        for (const auto &key : m_settings->allKeys())
            if (!values.contains(key)) m_settings->remove(key);
        for (auto it = values.values().cbegin(); it != values.values().cend(); ++it)
            if (m_settings->value(it.key()) != it.value()) m_settings->setValue(it.key(), it.value());
        m_settings->sync();
        return m_settings->status() == QSettings::NoError
            && QFileInfo(m_path).canonicalFilePath() == m_destination;
    }
private:
    bool unchanged() const
    {
        struct stat current {};
        if (m_existed) {
            if (::stat(QFile::encodeName(m_path).constData(), &current) != 0 || !sameSettingsFile(m_identity, current)) return false;
        } else if (QFileInfo::exists(m_path) || QFileInfo(m_path).isSymLink()) return false;
        return resolvedSettingsPath(m_path) == m_destination;
    }
    QTemporaryDir m_temporary;
    QFile m_source;
    QString m_path;
    QString m_destination;
    struct stat m_identity {};
    bool m_existed = false;
    std::unique_ptr<QSettings> m_settings;
};
inline bool acquireSettingsLock(QLockFile &lock, QElapsedTimer &waiting)
{
    // Short retries avoid starvation of a burst of independent writers.
    while (!lock.tryLock(0)) {
        if (lock.error() != QLockFile::LockFailedError || waiting.elapsed() >= 5000) return false;
        QThread::msleep(5);
    }
    return true;
}
inline bool edit(const std::function<void(SettingsValues &)> &change, bool discardLegacy = false,
                 int newLimit = -1, const ImagePathResolver &resolve = canonicalImagePath)
{
    const QString path = imageHistoryPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QElapsedTimer waiting;
    waiting.start();
    QLockFile historyLock(path + QStringLiteral(".history.lock"));
    if (!acquireSettingsLock(historyLock, waiting)) return false;
    // Coordinate with ordinary QSettings writers too. The alias has its own
    // private Qt lock, so syncing it cannot reacquire this original-file lock.
    QLockFile writerLock(path + QStringLiteral(".lock"));
    if (!acquireSettingsLock(writerLock, waiting)) return false;
    FreshSettings fresh(path, true);
    if (!fresh.get()) return false;
    SettingsValues settings(*fresh.get());
    const bool migrated = settings.value(QLatin1String(migratedKey), false).toBool();
    FreshSettings legacy(managerPath());
    if (!legacy.get() && !migrated) return false;
    if (newLimit >= 0) settings.setValue(QLatin1String(limitKey), qBound(0, newLimit, 9));
    auto images = settings.value(QLatin1String(imagesKey)).toStringList();
    if (!settings.value(QLatin1String(migratedKey), false).toBool()) {
        // Neither file has timestamps: preserve tray MRU first, then manager MRU.
        if (!discardLegacy && limit(settings) > 0)
            images += legacy.get()->value(QLatin1String(imagesKey)).toStringList();
        settings.setValue(QLatin1String(migratedKey), true);
    }
    writeImages(settings, normalized(discardLegacy ? QStringList{} : images, limit(settings), migrated ? ImagePathResolver{} : resolve));
    change(settings);
    writeImages(settings, normalized(settings.value(QLatin1String(imagesKey)).toStringList(), limit(settings)));
    if (!fresh.write(settings)) return false;
    // Commit list/marker first: failed legacy cleanup cannot resurrect history.
    // Malformed unrelated preferences stay byte-for-byte untouched after migration.
    writerLock.unlock();
    if (legacy.get() && legacy.get()->contains(QLatin1String(imagesKey))) {
        QLockFile legacyLock(managerPath() + QStringLiteral(".lock"));
        if (!acquireSettingsLock(legacyLock, waiting)) return false;
        FreshSettings cleanup(managerPath(), true);
        if (!cleanup.get()) return false;
        SettingsValues values(*cleanup.get());
        values.remove(QLatin1String(imagesKey));
        return cleanup.write(values);
    }
    return true;
}
inline bool ensureMigration(const ImagePathResolver &resolve = canonicalImagePath)
{
    FreshSettings fresh(imageHistoryPath());
    if (!fresh.get()) return false;
    const auto &settings = *fresh.get();
    const bool migrated = settings.value(QLatin1String(migratedKey), false).toBool();
    FreshSettings legacy(managerPath());
    if (!legacy.get() && !migrated) return false;
    if (migrated && (!legacy.get() || !legacy.get()->contains(QLatin1String(imagesKey)))
        && !(limit(settings) == 0 && settings.contains(QLatin1String(imagesKey)))) return true;
    return edit([](image_history_detail::SettingsValues &) {}, false, -1, resolve);
}
}
inline int recentImageLimit(const ImagePathResolver &resolve = canonicalImagePath)
{
    (void)image_history_detail::ensureMigration(resolve);
    image_history_detail::FreshSettings fresh(imageHistoryPath());
    return fresh.get() ? image_history_detail::limit(*fresh.get()) : 5;
}
inline bool setRecentImageLimit(int value)
{
    return image_history_detail::edit([](image_history_detail::SettingsValues &) {}, false, qBound(0, value, 9));
}
inline QStringList recentImages(const ImagePathResolver &resolve = canonicalImagePath)
{
    if (!image_history_detail::ensureMigration(resolve)) return {};
    image_history_detail::FreshSettings fresh(imageHistoryPath());
    if (!fresh.get()) return {};
    return image_history_detail::normalized(fresh.get()->value(QLatin1String(image_history_detail::imagesKey)).toStringList(), image_history_detail::limit(*fresh.get()));
}
inline bool rememberImage(const QString &path, const ImagePathResolver &resolve = canonicalImagePath)
{
    const auto image = resolve(path);
    if (image.isEmpty()) return false;
    return image_history_detail::edit([&](image_history_detail::SettingsValues &settings) {
        auto images = settings.value(QLatin1String(image_history_detail::imagesKey)).toStringList();
        images = image_history_detail::normalized(images, 9);
        images.removeAll(image);
        images.prepend(image);
        image_history_detail::writeImages(settings, images);
    }, false, -1, resolve);
}
inline bool forgetImage(const QString &path)
{
    const auto image = canonicalImagePath(path);
    if (image.isEmpty()) return false;
    return image_history_detail::edit([&](image_history_detail::SettingsValues &settings) {
        auto images = image_history_detail::normalized(settings.value(QLatin1String(image_history_detail::imagesKey)).toStringList(), 9);
        images.removeAll(image);
        image_history_detail::writeImages(settings, images);
    });
}
inline bool clearRecentImages()
{
    return image_history_detail::edit([](image_history_detail::SettingsValues &settings) { settings.remove(QLatin1String(image_history_detail::imagesKey)); }, true);
}
inline bool replaceRecentImage(const QString &oldPath, const QString &newPath)
{
    const auto oldImage = canonicalImagePath(oldPath), newImage = canonicalImagePath(newPath);
    if (oldImage.isEmpty() || newImage.isEmpty()) return false;
    return image_history_detail::edit([&](image_history_detail::SettingsValues &settings) {
        auto images = image_history_detail::normalized(settings.value(QLatin1String(image_history_detail::imagesKey)).toStringList(), 9);
        for (auto &image : images) if (image == oldImage) image = newImage;
        image_history_detail::writeImages(settings, images);
    });
}
}
