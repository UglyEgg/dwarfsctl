// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfs_worker.h"

#include "dwarfsctl_cat.h"
#include "error_mapping.h"
#include "dwarfsctl_ls.h"
#include "url_resolver.h"

#include <QDebug>

#include <QByteArray>
#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QString>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

#include <KLocalizedString>
#include <kio/udsentry.h>

using namespace DwarfsKio;

namespace {

/// Turn a backend failure into a KIO result via the shared mapping.
///
/// The classification itself lives in error_mapping.cpp so it can be unit-tested
/// without a D-Bus connection, a process, or an image -- and so there is exactly
/// one place where a backend kind becomes a KIO code.
KIO::WorkerResult backendFailure(const QString &kind, const QString &message, const QUrl &url)
{
    const ClassifiedFailure classified =
        classifyFailure(BackendFailure{kind, message}, url.toDisplayString());
    return KIO::WorkerResult::fail(classified.kioCode, classified.message);
}

KIO::WorkerResult backendFailure(const ListResult &result, const QUrl &url)
{
    return backendFailure(result.errorKind, result.error, url);
}

/// Refuse a write operation, saying why.
///
/// One helper rather than six near-identical bodies, so the message and the code
/// cannot drift apart between them.
KIO::WorkerResult refuseWrite(const QUrl &url)
{
    return KIO::WorkerResult::fail(
        KIO::ERR_UNSUPPORTED_ACTION,
        i18n("%1 is inside a DwarFS image, which is read-only by design; "
             "extract it to a normal folder before making changes",
             url.toDisplayString()));
}

/// Build the UDSEntry for one listed item.
KIO::UDSEntry toUdsEntry(const ListedEntry &entry)
{
    KIO::UDSEntry uds;
    uds.fastInsert(KIO::UDSEntry::UDS_NAME, entry.name);
    // A directory is reported with its file type only. KIO derives "is a folder"
    // from S_IFDIR, and a size on a directory confuses some consumers.
    if (entry.isDir) {
        uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFDIR);
    } else {
        uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
        if (entry.hasSize) {
            uds.fastInsert(KIO::UDSEntry::UDS_SIZE, entry.size);
        }
    }
    if (entry.hasMode) {
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS, entry.mode & 07777);
    }
    // No UDS_MODIFICATION_TIME and no UDS_ACCESS_TIME, deliberately: DwarFS's
    // directory_tree reports neither, and inventing a timestamp (the image's own
    // mtime, or epoch 0) would be worse than omitting it, because a consumer
    // cannot tell a real value from a placeholder.
    return uds;
}

} // namespace

// WorkerBase is constructed with the protocol name and the two sockets kiod
// opened. The protocol must match the scheme the worker serves.
DwarfsWorker::DwarfsWorker(const QByteArray &poolSocket, const QByteArray &appSocket)
    : KIO::WorkerBase("dwarfs", poolSocket, appSocket)
{
}

DwarfsWorker::~DwarfsWorker() = default;

// Enumerate a directory inside the image.
//
// The listing comes from `dwarfsctl ls`, never from the worker running DwarFS
// tools itself. Every image operation goes through the backend, which also keeps
// this component free of DwarFS knowledge.
//
// A failure is reported as a failure. Reporting an empty listing when the command
// failed would render an unreachable image as a valid empty directory, which is
// indistinguishable to the user from an image that is genuinely empty.
KIO::WorkerResult DwarfsWorker::listDir(const QUrl &url)
{
    const auto resolved = resolve(url);
    if (!resolved.ok) {
        return KIO::WorkerResult::fail(KIO::ERR_MALFORMED_URL, resolved.error);
    }

    const ListResult result = listImage(resolved.imagePath);
    if (!result.ok) {
        return backendFailure(result, url);
    }

    // The image root is the image itself; every other path is a directory inside
    // it. A path that is not in the listing is a genuine not-found rather than an
    // empty directory.
    if (resolved.innerPath != QLatin1String("/")) {
        const ListedEntry *self = entryAt(result.entries, resolved.innerPath);
        if (self == nullptr) {
            return KIO::WorkerResult::fail(
                KIO::ERR_DOES_NOT_EXIST,
                i18n("no such path in image: %1", resolved.innerPath));
        }
        if (!self->isDir) {
            return KIO::WorkerResult::fail(
                KIO::ERR_IS_FILE,
                i18n("%1 is not a directory inside the image", resolved.innerPath));
        }
    }

    const QVector<ListedEntry> children = childrenOf(result.entries, resolved.innerPath);
    KIO::UDSEntryList list;
    list.reserve(children.size());
    for (const ListedEntry &entry : children) {
        list.append(toUdsEntry(entry));
    }

    listEntries(list);
    return KIO::WorkerResult::pass();
}

KIO::WorkerResult DwarfsWorker::stat(const QUrl &url)
{
    const auto resolved = resolve(url);
    if (!resolved.ok) {
        return KIO::WorkerResult::fail(KIO::ERR_MALFORMED_URL, resolved.error);
    }

    // The protocol root describes the backing image file. URL syntax alone
    // does not establish that it exists; report only measured local metadata.
    if (resolved.innerPath == QLatin1String("/")) {
        struct stat info {};
        const auto path = QFile::encodeName(resolved.imagePath);
        if (::stat(path.constData(), &info) != 0) {
            const int reason = errno;
            const int code = reason == ENOENT || reason == ENOTDIR ? KIO::ERR_DOES_NOT_EXIST
                : reason == EACCES || reason == EPERM ? KIO::ERR_ACCESS_DENIED : KIO::ERR_CANNOT_STAT;
            return KIO::WorkerResult::fail(code,
                i18n("Could not inspect image %1: %2", resolved.imagePath,
                    QString::fromLocal8Bit(std::strerror(reason))));
        }
        if (!S_ISREG(info.st_mode))
            return KIO::WorkerResult::fail(S_ISDIR(info.st_mode) ? KIO::ERR_IS_DIRECTORY : KIO::ERR_CANNOT_STAT,
                i18n("The image path is not a regular file: %1", resolved.imagePath));
        KIO::UDSEntry uds;
        uds.fastInsert(KIO::UDSEntry::UDS_NAME, QUrl::fromLocalFile(resolved.imagePath).fileName());
        uds.fastInsert(KIO::UDSEntry::UDS_FILE_TYPE, S_IFREG);
        uds.fastInsert(KIO::UDSEntry::UDS_SIZE, info.st_size);
        uds.fastInsert(KIO::UDSEntry::UDS_ACCESS, info.st_mode & 07777);
        uds.fastInsert(KIO::UDSEntry::UDS_URL, url.toString());
        statEntry(uds);
        return KIO::WorkerResult::pass();
    }

    // Resolving one path must not re-list the whole image: this is on the hot
    // path for a directory browse, where Dolphin issues one listDir followed by a
    // stat() per entry. lookupEntry serves a warm cache outright and narrows to
    // `ls --path ... --entry-only` on a cold one.
    const ListResult result = lookupEntry(resolved.imagePath, resolved.innerPath);
    if (!result.ok) {
        return backendFailure(result, url);
    }

    // lookupEntry returns exactly the requested entry; a successful result that
    // does not carry it would be a contract violation, not a not-found.
    const ListedEntry *entry = entryAt(result.entries, resolved.innerPath);
    if (entry == nullptr) {
        return KIO::WorkerResult::fail(
            KIO::ERR_DOES_NOT_EXIST,
            i18n("no such path in image: %1", resolved.innerPath));
    }

    KIO::UDSEntry uds = toUdsEntry(*entry);
    uds.fastInsert(KIO::UDSEntry::UDS_URL, url.toString());
    statEntry(uds);
    return KIO::WorkerResult::pass();
}

// Serve file contents.
//
// The bytes come from `dwarfsctl cat`, which materialises exactly one file into a
// private temp path. The worker never runs DwarFS tools itself, so the
// two traps in that tool -- it cannot stream to stdout, and its `--pattern`
// silently matches nothing when given an absolute path while still exiting 0 --
// are handled and tested in the backend rather than here.
//
// Two properties worth keeping:
//
//  - A missing or unreadable file is an error, never an empty success. The backend
//    reports "no file was produced", and that is passed through.
//  - The temp file is released on EVERY path after materialise() succeeds,
//    including when the read fails, so a broken image cannot leak a temp file per
//    attempt.
KIO::WorkerResult DwarfsWorker::get(const QUrl &url)
{
    const auto resolved = resolve(url);
    if (!resolved.ok) {
        return KIO::WorkerResult::fail(KIO::ERR_MALFORMED_URL, resolved.error);
    }

    // Reading the image root (or a directory) is not a file read. Refuse with the
    // specific code rather than attempting an extraction that cannot work.
    if (resolved.innerPath == QLatin1String("/")) {
        return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY,
                                       i18n("the image itself is not a file"));
    }

    MaterialisedFile file = materialise(resolved.imagePath, resolved.innerPath);
    if (!file.ok) {
        // Same single mapping as every other failure path.
        return backendFailure(file.errorKind, file.error, url);
    }

    // KIO requires the content type before the first data block. Prefer the
    // original name because the private materialisation path intentionally has
    // no useful extension; fall back to inspecting the bytes for extensionless
    // or unknown files.
    QMimeDatabase mimeDatabase;
    QMimeType contentType =
        mimeDatabase.mimeTypeForFile(resolved.innerPath, QMimeDatabase::MatchExtension);
    if (!contentType.isValid() || contentType.isDefault()) {
        contentType = mimeDatabase.mimeTypeForFile(file.localPath, QMimeDatabase::MatchContent);
    }
    mimeType(contentType.isValid() ? contentType.name()
                                   : QStringLiteral("application/octet-stream"));

    // Tell the caller how big the file is before sending it, so a consumer can show
    // progress and a partial transfer is detectable.
    totalSize(file.size);

    // Stream in bounded chunks rather than loading the whole file: an image can
    // hold something far larger than should ever be resident, and Dolphin will ask
    // for all of it.
    QString readError;
    const bool readOk = streamFile(file.localPath, [&](const QByteArray &chunk) {
        data(chunk);
        return true;
    });

    // Release regardless of outcome: this is the only place the temp file is
    // cleaned up, so returning early above would leak it.
    releaseMaterialised(file);

    if (!readOk) {
        return KIO::WorkerResult::fail(
            KIO::ERR_CANNOT_READ,
            i18n("the file was extracted but could not be read back"));
    }

    return KIO::WorkerResult::pass();
}

// Copy one file from a browsable image straight to the local filesystem.
//
// KIO otherwise falls back to get()+put(): the worker first extracts into its
// private temp directory, streams every byte through KIO, and the file worker
// writes those bytes a second time. The backend already has an atomic, no-clobber
// local publication contract, so using it directly avoids that duplicate I/O
// while retaining overwrite safety and cancellation.
KIO::WorkerResult DwarfsWorker::copy(const QUrl &src,
                                     const QUrl &dest,
                                     int permissions,
                                     KIO::JobFlags flags)
{
    if (!dest.isLocalFile()) {
        return KIO::WorkerResult::fail(
            KIO::ERR_UNSUPPORTED_ACTION,
            i18n("DwarFS images can only copy directly to a local folder"));
    }

    const auto resolved = resolve(src);
    if (!resolved.ok) {
        return KIO::WorkerResult::fail(KIO::ERR_MALFORMED_URL, resolved.error);
    }
    if (resolved.innerPath == QLatin1String("/")) {
        return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY,
                                       i18n("the image itself is not a file"));
    }

    const QString destination = dest.toLocalFile();
    const bool overwrite = flags.testFlag(KIO::Overwrite);
    const CopyOutResult result = copyOutToLocal(
        resolved.imagePath,
        resolved.innerPath,
        destination,
        overwrite,
        [this]() { return wasKilled(); });
    if (!result.ok) {
        if (result.errorKind == QLatin1String("DestinationExists")) {
            return KIO::WorkerResult::fail(KIO::ERR_FILE_ALREADY_EXIST, destination);
        }
        if (result.errorKind == QLatin1String("DestinationIsDirectory")) {
            return KIO::WorkerResult::fail(KIO::ERR_IS_DIRECTORY, destination);
        }
        if (result.errorKind == QLatin1String("Cancelled")) {
            return KIO::WorkerResult::fail(KIO::ERR_USER_CANCELED, result.error);
        }
        return backendFailure(result.errorKind, result.error, src);
    }

    totalSize(result.size);
    processedSize(result.size);

    if (permissions != -1) {
        const QByteArray encoded = QFile::encodeName(destination);
        if (::chmod(encoded.constData(), static_cast<mode_t>(permissions & 07777)) != 0) {
            return KIO::WorkerResult::fail(
                KIO::ERR_CANNOT_CHMOD,
                i18n("could not set permissions on %1: %2",
                     destination,
                     QString::fromLocal8Bit(std::strerror(errno))));
        }
    }
    return KIO::WorkerResult::pass();
}

// ---------------------------------------------------------------------------
// Write operations
// ---------------------------------------------------------------------------
// DwarFS images are read-only by design: there is no code path that could write
// inside one, and inventing one would mean a second writer for a format that has
// no writable in-place representation.
//
// These are overridden rather than left to KIO's defaults for two reasons: the
// refusal becomes a decision visible in this file rather than a property of a
// default that could change, and Dolphin learns the actions are unavailable so it
// greys them out instead of letting the user try and fail.
//
// The remedy in the message matters: extracting to a normal folder is the
// supported way to make changes, so the refusal points at it rather than just
// saying no.

KIO::WorkerResult DwarfsWorker::put(const QUrl &url, int permissions, KIO::JobFlags flags)
{
    Q_UNUSED(permissions);
    Q_UNUSED(flags);
    return refuseWrite(url);
}

KIO::WorkerResult DwarfsWorker::mkdir(const QUrl &url, int permissions)
{
    Q_UNUSED(permissions);
    return refuseWrite(url);
}

KIO::WorkerResult DwarfsWorker::del(const QUrl &url, bool isfile)
{
    Q_UNUSED(isfile);
    return refuseWrite(url);
}

KIO::WorkerResult DwarfsWorker::rename(const QUrl &src, const QUrl &dest, KIO::JobFlags flags)
{
    Q_UNUSED(dest);
    Q_UNUSED(flags);
    return refuseWrite(src);
}

KIO::WorkerResult DwarfsWorker::symlink(const QString &target, const QUrl &dest, KIO::JobFlags flags)
{
    Q_UNUSED(target);
    Q_UNUSED(flags);
    return refuseWrite(dest);
}

KIO::WorkerResult DwarfsWorker::chmod(const QUrl &url, int permissions)
{
    Q_UNUSED(permissions);
    return refuseWrite(url);
}
