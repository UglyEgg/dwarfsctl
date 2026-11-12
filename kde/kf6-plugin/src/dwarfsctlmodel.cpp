// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctlmodel.h"

#include <QJsonValue>
#include <QJsonArray>

#include <functional>

namespace dwarfsplugin {

static DecodeResult ok() {
    DecodeResult r;
    r.ok = true;
    return r;
}

static DecodeResult err(const QString &msg) {
    DecodeResult r;
    r.ok = false;
    r.error_message = msg;
    return r;
}

DecodeResult decodeStatus(const Envelope &env, StatusDetails *out)
{
    if (!out) {
        return err(QStringLiteral("StatusDetails output pointer was null"));
    }
    if (env.details.isEmpty()) {
        return err(QStringLiteral("Envelope missing 'details'"));
    }

    // mounted is optional in the public schema; only present when an image was provided.
    const QJsonValue mv = env.details.value(QStringLiteral("mounted"));
    if (mv.isBool()) {
        out->mounted_present = true;
        out->mounted = mv.toBool();
    }

    out->mountpoint = env.details.value(QStringLiteral("mountpoint")).toString();
    out->mount_health = env.details.value(QStringLiteral("mount_health")).toString();
    out->mount_fs_type = env.details.value(QStringLiteral("mount_fs_type")).toString();
    out->mount_source = env.details.value(QStringLiteral("mount_source")).toString();
    if (out->mounted_present && !out->mounted) {
        // `status` also reports the path it would use under image.paths. A
        // non-mounted image must not present that derived path as an active
        // mountpoint.
        out->mountpoint.clear();
        out->mount_health.clear();
        out->mount_fs_type.clear();
        out->mount_source.clear();
    }

    // deps (dynamic)
    out->deps.clear();
    const QJsonValue depsv = env.details.value(QStringLiteral("deps"));
    if (depsv.isObject()) {
        const QJsonObject deps = depsv.toObject();
        for (auto it = deps.begin(); it != deps.end(); ++it) {
            if (!it.value().isObject()) {
                continue;
            }
            const QJsonObject d = it.value().toObject();
            DepInfo info;
            info.found = d.value(QStringLiteral("found")).toBool(false);
            info.path = d.value(QStringLiteral("path")).toString();
            info.installed_version = d.value(QStringLiteral("installed_version")).toString();
            out->deps.insert(it.key(), info);
        }
    }

    // image.paths
    const QJsonValue imagev = env.details.value(QStringLiteral("image"));
    if (imagev.isObject()) {
        const QJsonObject image = imagev.toObject();
        const QJsonValue pathsv = image.value(QStringLiteral("paths"));
        if (pathsv.isObject()) {
            const QJsonObject paths = pathsv.toObject();
            out->canonical_image = paths.value(QStringLiteral("canonical")).toString();
            out->image_hash = paths.value(QStringLiteral("hash")).toString();
            out->derived_mountpoint = paths.value(QStringLiteral("mountpoint")).toString();
            out->lock_path = paths.value(QStringLiteral("lock_path")).toString();
        }
    }

    // Rich image metadata lives in the backend-owned dwarfsck payload. The
    // tray intentionally returns only mount state, so absence here is valid.
    QJsonObject toolPayload;
    const QJsonValue toolsValue = env.details.value(QStringLiteral("tools"));
    if (toolsValue.isObject()) {
        const QJsonValue dwarfsck = toolsValue.toObject().value(QStringLiteral("dwarfsck"));
        if (dwarfsck.isObject()) {
            toolPayload = dwarfsck.toObject();
        }
    }
    if (toolPayload.isEmpty()) {
        const QJsonValue legacy = env.details.value(QStringLiteral("dwarfsck"));
        if (legacy.isObject()) {
            toolPayload = legacy.toObject();
        }
    }

    const QJsonValue metadataValue = toolPayload.value(QStringLiteral("json"));
    if (metadataValue.isObject()) {
        const QJsonObject metadata = metadataValue.toObject();
        out->image_metadata_present = true;
        out->created_by = metadata.value(QStringLiteral("created_by")).toString();
        out->created_on = metadata.value(QStringLiteral("created_on")).toString();
        out->inode_count = static_cast<qint64>(metadata.value(QStringLiteral("inode_count")).toDouble(-1));
        out->original_size = static_cast<qint64>(metadata.value(QStringLiteral("original_filesystem_size")).toDouble(-1));
        out->block_count = static_cast<qint64>(metadata.value(QStringLiteral("block_count")).toDouble(-1));
        out->block_size = static_cast<qint64>(metadata.value(QStringLiteral("block_size")).toDouble(-1));

        const QJsonValue versionValue = metadata.value(QStringLiteral("version"));
        if (versionValue.isObject()) {
            const QJsonObject version = versionValue.toObject();
            const int major = version.value(QStringLiteral("major")).toInt(-1);
            const int minor = version.value(QStringLiteral("minor")).toInt(-1);
            const int header = version.value(QStringLiteral("header")).toInt(-1);
            if (major >= 0 && minor >= 0) {
                out->format_version = QStringLiteral("%1.%2").arg(major).arg(minor);
                if (header >= 0) {
                    out->format_version += QStringLiteral(" (header %1)").arg(header);
                }
            }
        }

        const QJsonValue featuresValue = metadata.value(QStringLiteral("features"));
        if (featuresValue.isArray()) {
            for (const QJsonValue &feature : featuresValue.toArray()) {
                if (feature.isString()) {
                    out->features.append(feature.toString());
                }
            }
        }

        qint64 files = 0;
        qint64 directories = 0;
        const QJsonValue rootValue = metadata.value(QStringLiteral("root"));
        if (rootValue.isObject()) {
            std::function<void(const QJsonObject &, bool)> countEntry;
            countEntry = [&](const QJsonObject &entry, bool isRoot) {
                const QString type = entry.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("directory")) {
                    if (!isRoot) {
                        ++directories;
                    }
                } else if (type == QStringLiteral("file")) {
                    ++files;
                }
                const QJsonValue children = entry.value(QStringLiteral("inodes"));
                if (children.isArray()) {
                    for (const QJsonValue &child : children.toArray()) {
                        if (child.isObject()) {
                            countEntry(child.toObject(), false);
                        }
                    }
                }
            };
            countEntry(rootValue.toObject(), true);
            out->file_count = files;
            out->directory_count = directories;
        }
    }

    return ok();
}


} // namespace dwarfsplugin
