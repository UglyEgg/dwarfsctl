// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "error_mapping.h"

#include <KLocalizedString>

namespace DwarfsKio {

namespace {

/// Ensure the translation domain is set before any i18n() call in this file.
///
/// A KLocalizedString call with no domain produces the string untranslated *and*
/// emits a warning per call. The domain has to be set by whoever loads these
/// strings; for a KIO worker that is this project, since the worker runs inside
/// kiod which knows nothing about it.
///
/// Done here, at the point of use, rather than only in the factory constructor:
/// the factory is not constructed by the unit tests, so setting it there left
/// every test warning on every call. Setting it in the translation unit that owns
/// the strings covers both, and costs one atomic load after the first call.
void ensureDomain()
{
    static const bool once = [] {
        KLocalizedString::setApplicationDomain("dwarfsctl");
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace

ClassifiedFailure classifyFailure(const BackendFailure &failure, const QString &artifact)
{
    ensureDomain();

    ClassifiedFailure out;

    // Every branch names the artifact: "the image could not be read" is far less
    // useful than naming which file, especially since a URL can point at any path.
    const QString subject = artifact.isEmpty() ? i18n("the image") : artifact;

    if (failure.kind == QLatin1String("InvalidPath")) {
        // The path or the image is not there. Distinct from "unreadable", because
        // the user's next action differs: check the path vs check permissions.
        out.kioCode = KIO::ERR_DOES_NOT_EXIST;
        out.message = failure.message.isEmpty()
            ? i18n("%1 does not exist", subject)
            : failure.message;
        return out;
    }

    if (failure.kind == QLatin1String("DependencyMissing")) {
        // A tool this backend drives is missing, or dwarfsctl itself. Naming the
        // remedy matters: the user cannot fix this by retrying, and `dwarfsctl
        // doctor` reports exactly the same set of tools.
        out.kioCode = KIO::ERR_CANNOT_LAUNCH_PROCESS;
        out.message = failure.message.isEmpty()
            ? i18n("a required DwarFS tool is missing; run \"dwarfsctl doctor\" to see which")
            : failure.message;
        return out;
    }

    if (failure.kind == QLatin1String("LockBusy")) {
        // Another operation holds the image. KIO has no ERR_BUSY; the closest
        // honest signal is ERR_SERVICE_NOT_AVAILABLE, which says "not right now"
        // without implying the user lacks permission (ERR_ACCESS_DENIED would
        // suggest a fix that does not exist, since the user owns the image).
        out.kioCode = KIO::ERR_SERVICE_NOT_AVAILABLE;
        out.message = failure.message.isEmpty()
            ? i18n("%1 is busy with another operation", subject)
            : failure.message;
        return out;
    }

    if (failure.kind == QLatin1String("MountStateError")) {
        // Mount state is the tray's business, not the worker's. If it surfaces
        // here something is wrong upstream of this component, so report it as an
        // internal problem rather than pretending it is an access error.
        out.kioCode = KIO::ERR_INTERNAL;
        out.message = failure.message.isEmpty()
            ? i18n("%1 could not be read (mount state problem reported by dwarfsctl)", subject)
            : failure.message;
        return out;
    }

    if (failure.kind == QLatin1String("BadCliUsage")) {
        // This means the worker built an argv dwarfsctl rejected. That is our bug,
        // and it should read as one rather than as a user-facing error.
        out.kioCode = KIO::ERR_INTERNAL;
        out.message = i18n("internal error: dwarfsctl rejected the request for %1", subject);
        return out;
    }

    if (failure.kind == QLatin1String("EmptyOutput")
        || failure.kind == QLatin1String("MalformedEnvelope")) {
        // The tool ran but produced something unusable. This must never render as
        // an empty directory: a listing that cannot be read and an image with no
        // contents are different facts. Quote the backend, which quotes the tool.
        out.kioCode = KIO::ERR_INTERNAL;
        out.message = failure.message.isEmpty()
            ? i18n("the contents of %1 could not be read", subject)
            : failure.message;
        return out;
    }

    if (failure.kind == QLatin1String("Timeout")) {
        out.kioCode = KIO::ERR_SERVER_TIMEOUT;
        out.message = failure.message.isEmpty()
            ? i18n("reading %1 took too long and was abandoned", subject)
            : failure.message;
        return out;
    }

    // Anything else, including an empty kind (a failure that did not come from the
    // envelope at all, such as a binary that could not be started).
    out.kioCode = KIO::ERR_INTERNAL;
    out.message = failure.message.isEmpty()
        ? i18n("%1 could not be read, and no reason was reported", subject)
        : failure.message;
    return out;
}

} // namespace DwarfsKio
