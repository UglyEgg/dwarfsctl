// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads an image listing from `dwarfsctl ls`.
//
// The worker deliberately knows NOTHING about DwarFS or about `dwarfsck`. It runs
// one backend command and reads one JSON envelope. Every operation that touches
// an image goes through `dwarfsctl`, so that tool lookup,
// error classification and DwarFS-version handling stay in one place instead of
// being re-implemented per KDE component.
//
// Consequence worth stating: the C++ side has no DwarFS tool dependencies at all,
// which is why its tests run anywhere. The parse of dwarfsck's own nested tree
// lives in the backend (`cpp/cli/operations.cpp`), where it is tested against a
// fixture captured from the real tool.

#pragma once

#include <QString>
#include <QVector>

namespace DwarfsKio {

/// One entry from a `dwarfsctl ls` envelope.
struct ListedEntry {
    QString name;
    /// Absolute path inside the image, starting with '/'.
    QString path;
    bool isDir = false;
    /// Absent for directories. Note Some(0) and absent are different facts: a
    /// zero-byte file is `hasSize && size == 0`, a directory is `!hasSize`.
    bool hasSize = false;
    quint64 size = 0;
    /// Unix mode bits, or nullopt when the backend could not read them.
    bool hasMode = false;
    quint32 mode = 0;
};

struct ListResult {
    bool ok = false;
    QVector<ListedEntry> entries;
    /// Populated when ok is false. Names the artifact and the reason, taken from
    /// the backend envelope rather than invented here.
    QString error;
    /// The backend's error kind (e.g. "InvalidPath", "DependencyMissing"), so
    /// callers can map to KIO codes without matching on message text.
    QString errorKind;
};

/// Run `dwarfsctl ls <image>` and parse the envelope.
///
/// Never returns ok with an empty list when the call failed: a failure is always
/// `ok == false` with a reason. An empty-but-successful list means the image
/// genuinely has no entries, and those two states must stay distinguishable --
/// conflating them renders an unreachable image as a valid empty directory.
///
/// The successful listing is remembered for this image (keyed by the image
/// file's size and modification time), because a directory browse issues one
/// listing followed by a `stat()` per entry, and re-listing the whole image per
/// `stat()` made entering an 80-file directory cost ~80 backend calls (review
/// finding F4).
ListResult listImage(const QString &imagePath, int timeoutMs = 300000);

/// Resolve exactly one entry by path -- the `stat()` primitive.
///
/// When the full listing for this image is already cached and the image file is
/// unchanged, the entry resolves from that listing with **no backend call at
/// all**. On a cold cache it runs
/// `dwarfsctl ls --path <innerPath> --entry-only <image>`, which returns only the
/// requested entry instead of the whole tree.
///
/// A path that is absent from a valid cached listing is a failure
/// (`errorKind == "InvalidPath"`) with no process run: the cached listing is
/// complete, so absence there is genuine absence -- never an empty success.
ListResult lookupEntry(const QString &imagePath, const QString &innerPath, int timeoutMs = 300000);

/// Drop every cached listing.
///
/// Exposed for tests: the cache is process-lifetime state, so a test that counts
/// backend invocations must start from a known-empty cache rather than depending
/// on the order suites run in.
void clearListingCache();

/// Parse an envelope that has already been captured. Exposed for testing, so the
/// parser can be exercised against recorded backend output with no process run.
ListResult parseEnvelope(const QByteArray &stdoutBytes, int exitCode, const QByteArray &stderrBytes);

/// The entries that are direct children of `innerPath`.
QVector<ListedEntry> childrenOf(const QVector<ListedEntry> &entries, const QString &innerPath);

/// Find one entry by absolute path. Returns nullptr when absent.
const ListedEntry *entryAt(const QVector<ListedEntry> &entries, const QString &innerPath);

} // namespace DwarfsKio
