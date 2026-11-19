// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFile>
#include <QString>
#include <optional>
#include <sys/stat.h>

namespace dwarfsctl_kde {

// A change detector, not an immutable snapshot of the file's contents.
struct FileIdentity {
    dev_t device;
    ino_t inode;
    off_t size;
    time_t mtimeSeconds;
    long mtimeNanoseconds;
    time_t ctimeSeconds;
    long ctimeNanoseconds;
    bool operator==(const FileIdentity &) const = default;
};

inline std::optional<FileIdentity> fileIdentity(const QString &path)
{
    struct stat info {};
    if (::stat(QFile::encodeName(path).constData(), &info) != 0 || !S_ISREG(info.st_mode))
        return std::nullopt;
    return FileIdentity{info.st_dev, info.st_ino, info.st_size,
                        info.st_mtim.tv_sec, info.st_mtim.tv_nsec,
                        info.st_ctim.tv_sec, info.st_ctim.tv_nsec};
}

} // namespace dwarfsctl_kde
