// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dwarfsctljson.h"

#include <QString>
#include <QStringList>
#include <QMap>

namespace dwarfsplugin {

struct DepInfo {
    bool found = false;
    QString path;
    QString installed_version;
};

struct StatusDetails {
    // Mount state
    bool mounted_present = false;
    bool mounted = false;
    QString mountpoint;
    QString mount_health;
    QString mount_fs_type;
    QString mount_source;

    // Dependency status (dynamic; keys come from dwarfsctl schema/deps output)
    QMap<QString, DepInfo> deps;

    // Derived paths (present when `dwarfsctl status <image>` succeeds)
    QString canonical_image;
    QString image_hash;
    QString derived_mountpoint;
    QString lock_path;

    // Filesystem metadata from `dwarfsctl status <image>`. These remain
    // absent for the tray's lightweight mount-only status envelope.
    bool image_metadata_present = false;
    QString created_by;
    QString created_on;
    QString format_version;
    QStringList features;
    qint64 inode_count = -1;
    qint64 file_count = -1;
    qint64 directory_count = -1;
    qint64 original_size = -1;
    qint64 block_count = -1;
    qint64 block_size = -1;
};




struct DecodeResult {
    bool ok = false;
    QString error_message;
};

DecodeResult decodeStatus(const Envelope &env, StatusDetails *out);

} // namespace dwarfsplugin
