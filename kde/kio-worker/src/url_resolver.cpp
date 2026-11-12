// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "url_resolver.h"

#include <QStringList>

namespace DwarfsKio {

namespace {

constexpr auto kScheme = "dwarfs";
constexpr auto kSuffix = ".dwarfs";

// Splits a path into components, dropping empty ones so that "//" collapses.
QStringList components(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

} // namespace

ResolvedUrl resolve(const QUrl &url)
{
    ResolvedUrl out;

    if (url.scheme() != QLatin1String(kScheme)) {
        out.error = QStringLiteral("not a dwarfs: URL (scheme was '%1')").arg(url.scheme());
        return out;
    }

    // A host component would make the path ambiguous (dwarfs://host/path); the
    // only supported form is dwarfs:///absolute/path.
    if (!url.host().isEmpty()) {
        out.error = QStringLiteral("dwarfs: URLs must not carry a host component; use dwarfs:///absolute/path");
        return out;
    }

    // QUrl::path() is percent-decoded, so spaces and non-ASCII arrive intact.
    const QString decoded = url.path();
    if (decoded.isEmpty()) {
        out.error = QStringLiteral("dwarfs: URL has no path");
        return out;
    }

    if (!decoded.startsWith(QLatin1Char('/'))) {
        out.error = QStringLiteral("image path must be absolute");
        return out;
    }

    const QStringList parts = components(decoded);

    // Find the first component that names the image. Everything before it is
    // directories leading to the image; everything after is inside the image.
    int imageIndex = -1;
    for (int i = 0; i < parts.size(); ++i) {
        if (parts.at(i).endsWith(QLatin1String(kSuffix), Qt::CaseInsensitive)) {
            imageIndex = i;
            break;
        }
    }

    if (imageIndex < 0) {
        out.error = QStringLiteral("no .dwarfs image in path; a dwarfs: URL must name an image file");
        return out;
    }

    // Rebuild the image path from its components, so a doubled slash or a
    // trailing slash cannot survive into the filesystem call.
    QStringList imageParts = parts.mid(0, imageIndex + 1);
    // Reject traversal in the path LEADING to the image: it would let a URL
    // address a different image than it appears to.
    for (const QString &p : imageParts) {
        if (p == QLatin1String(".") || p == QLatin1String("..")) {
            out.error = QStringLiteral("path to the image must not contain '.' or '..'");
            return out;
        }
    }
    out.imagePath = QLatin1Char('/') + imageParts.join(QLatin1Char('/'));

    // The inner path is the remainder. Traversal here is the dangerous case:
    // "../x" must not be able to reach outside the image.
    const QStringList innerParts = parts.mid(imageIndex + 1);

    // A path component containing a slash cannot occur after splitting, but a
    // component that IS ".." or "." can, and either must be rejected outright
    // rather than normalised away -- silent normalisation would make
    // "img.dwarfs/a/../b" address "img.dwarfs/b", which is a different file than
    // the URL names.
    for (const QString &p : innerParts) {
        if (p == QLatin1String("..") || p == QLatin1String(".")) {
            out.error = QStringLiteral("path inside the image must not contain '.' or '..' components");
            return out;
        }
    }

    out.innerPath = QLatin1Char('/') + innerParts.join(QLatin1Char('/'));
    out.ok = true;
    return out;
}

QUrl toUrl(const QString &imagePath, const QString &innerPath)
{
    QString path = imagePath;
    if (!innerPath.isEmpty() && innerPath != QLatin1String("/")) {
        // innerPath always starts with '/', so this appends cleanly.
        path += innerPath;
    }

    QUrl url;
    url.setScheme(QLatin1String(kScheme));

    // DecodedMode is the one that matters here, and it is also QUrl's default.
    // In DecodedMode setPath() percent-encodes what needs encoding, so a space
    // becomes %20 and non-ASCII becomes UTF-8 percent-triplets -- exactly what
    // resolve() decodes back. TolerantMode instead assumes the input may
    // ALREADY be encoded and leaves a raw space in place, producing a URL whose
    // toString() is not a usable URL.
    url.setPath(path, QUrl::DecodedMode);
    return url;
}

} // namespace DwarfsKio
