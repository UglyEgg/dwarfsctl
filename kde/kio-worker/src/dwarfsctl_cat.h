// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads file contents out of an image, via `dwarfsctl cat`.
//
// The worker never runs DwarFS tools itself: it asks the backend to
// materialise one file into a private temp path, then serves those bytes and
// removes the temp file. Exact image lookup, extraction behavior and atomic
// publication remain backend responsibilities and are tested there.
//
// The temp file is removed on every path, including failure, and the read is
// bounded so a very large file inside an image cannot be pulled into memory whole.

#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace DwarfsKio {

/// Result of materialising one file from an image.
struct MaterialisedFile {
    bool ok = false;
    /// Local path holding the file's bytes. Valid until release() is called.
    QString localPath;
    /// Size in bytes, or 0 for a legitimately empty file.
    quint64 size = 0;
    /// Populated when ok is false; the backend's own message, naming the artifact.
    QString error;
    QString errorKind;
};

/// Result of publishing one image member directly to a local destination.
struct CopyOutResult {
    bool ok = false;
    quint64 size = 0;
    QString error;
    QString errorKind;
};

/// Ask the backend to write `innerPath` from `imagePath` into a private temp file.
///
/// The caller owns the temp file and must call releaseMaterialised() when done,
/// including on failure paths after a successful call.
MaterialisedFile materialise(const QString &imagePath, const QString &innerPath);

/// Copy one image member directly to a local path through dwarfsctl.
///
/// This uses the backend's staged, atomic publication instead of materialising
/// into a KIO temp directory and copying the bytes a second time. Existing
/// destinations are preserved unless overwrite is true. The cancellation hook
/// is polled while the backend runs and may be empty when cancellation is not
/// needed.
CopyOutResult copyOutToLocal(const QString &imagePath,
                             const QString &innerPath,
                             const QString &destination,
                             bool overwrite,
                             const std::function<bool()> &cancelled = {});

/// Delete a file produced by materialise(), and the directory holding it.
void releaseMaterialised(const MaterialisedFile &file);

/// Read a whole file into memory, refusing absurd sizes.
///
/// `maxBytes` exists because an image can contain a file far larger than anything
/// a caller wants in RAM at once. Exceeding it is an explicit error rather than a
/// silent truncation: handing back a partial file while reporting success is the
/// fabricated-success class this project has already shipped twice.
bool readWholeFile(const QString &path, quint64 size, QByteArray *out, quint64 maxBytes);

/// Stream a file in bounded chunks.
///
/// `emit` is called with each chunk and returns false to stop early (e.g. the
/// caller was cancelled). Returns false if the file could not be read.
bool streamFile(const QString &path, const std::function<bool(const QByteArray &)> &emit);

} // namespace DwarfsKio
