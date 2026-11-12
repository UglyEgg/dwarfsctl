// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Maps backend failures to KIO results, in one place.
//
// Why this is separate from the worker: the mapping is pure logic (a kind and a
// message in, a code and a message out), so it can be unit-tested without a
// D-Bus connection, a process, or an image. That matters because the mapping is
// what a user actually sees when something goes wrong -- and a wrong code means
// Dolphin shows the wrong thing, or offers an action that cannot work.
//
// The mapping keys on the backend's `error.kind`, never on message text. A
// reworded backend message must not silently change the classification.

#pragma once

#include <QString>

#include <kio/workerbase.h>

namespace DwarfsKio {

/// What the backend said went wrong, plus its message.
///
/// Mirrors the envelope's `error` object. `kind` is empty when the failure came
/// from somewhere that does not speak the envelope (a missing binary, a timeout).
struct BackendFailure {
    QString kind;
    QString message;
};

/// Result of classifying a failure.
struct ClassifiedFailure {
    int kioCode = KIO::ERR_INTERNAL;
    /// What the user is shown. Never empty: a failure with no explanation is
    /// worse than a vague one, because it looks like a bug in the file manager.
    QString message;
};

/// Classify a backend failure into a KIO code plus a message worth reading.
///
/// `artifact` is the image or path the operation concerned, used when the
/// backend's own message does not name it.
ClassifiedFailure classifyFailure(const BackendFailure &failure, const QString &artifact);

} // namespace DwarfsKio
