// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// URL shape for the dwarfs: scheme.
//
// A URL is:  dwarfs:///absolute/path/to/image.dwarfs[/inner/path]
//
// The image path ends at the first path component ending in `.dwarfs`; everything
// after it is a path INSIDE the image. This split has to happen before any I/O,
// because the inner path is attacker-influenced (it comes from a URL) and must
// not be able to escape the image.
//
// This file is deliberately free of KIO and of any process execution: the rules
// here are pure logic, so they are unit-testable everywhere and the security
// decisions are visible in one place rather than scattered through the worker.

#pragma once

#include <QString>
#include <QUrl>

namespace DwarfsKio {

struct ResolvedUrl {
    // True when the URL was well formed and safe to act on.
    bool ok = false;
    // Absolute path to the image on the local filesystem.
    QString imagePath;
    // Normalised path inside the image. Always starts with '/'. "/" means the
    // image root itself.
    QString innerPath;
    // Human-readable reason when ok == false. Names the offending part.
    QString error;
};

// Splits a dwarfs: URL into the image path and the path inside the image.
//
// Rejects, with a specific error:
//   - a non-dwarfs scheme, or a relative/host-carrying form
//   - an image path that is not absolute
//   - an image component that is not an absolute path
//   - any inner path that would escape the image (`.`/`..` traversal)
//   - a `.` path component anywhere in the inner path
ResolvedUrl resolve(const QUrl &url);

// Builds the canonical dwarfs: URL for an image plus an inner path. Used for the
// worker's reported URLs and for tests to prove the round trip is lossless.
QUrl toUrl(const QString &imagePath, const QString &innerPath);

} // namespace DwarfsKio
