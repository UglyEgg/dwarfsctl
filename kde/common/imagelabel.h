// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QSet>

namespace dwarfsctl_kde {
// Return the shortest parent suffix that distinguishes equal display names.
// Full paths remain available in the owning view's tooltip/details. Equal stems
// in the same parent need a filename-inclusive suffix (for example .dwarfs/.DWARFS).
inline QString imageParentHint(const QString &path, const QStringList &peers)
{
    const QFileInfo image(path);
    const QString parent = QDir::cleanPath(image.absolutePath());
    const QString identity = QDir::cleanPath(image.absoluteFilePath());
    QStringList parents;
    QStringList paths;
    QSet<QString> seenParents{parent};
    bool includeFilename = false;
    for (const auto &peerPath : peers) {
        const QFileInfo peer(peerPath);
        if (QDir::cleanPath(peer.absoluteFilePath()) != identity
            && peer.completeBaseName() == image.completeBaseName()) {
            const QString peerParent = QDir::cleanPath(peer.absolutePath());
            if (seenParents.contains(peerParent)) includeFilename = true;
            seenParents.insert(peerParent);
            parents.append(peerParent);
            paths.append(QDir::cleanPath(peer.absoluteFilePath()));
        }
    }
    if (parents.isEmpty()) return {};
    const QString subject = includeFilename ? identity : parent;
    const QStringList competitors = includeFilename ? paths : parents;
    const auto suffix = [](const QString &directory, int depth) {
        const auto parts = directory.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        return parts.isEmpty() ? QStringLiteral("/") : parts.mid(qMax<qsizetype>(0, parts.size() - depth)).join(QLatin1Char('/'));
    };
    qsizetype maximum = subject.count(QLatin1Char('/')) + 1;
    for (const auto &directory : competitors) maximum = qMax(maximum, directory.count(QLatin1Char('/')) + 1);
    for (int depth = 1; depth <= maximum; ++depth) {
        const auto hint = suffix(subject, depth);
        bool unique = true;
        for (const auto &directory : competitors) {
            if (suffix(directory, depth) == hint) { unique = false; break; }
        }
        if (unique) return QDir::toNativeSeparators(hint);
    }
    return QDir::toNativeSeparators(subject);
}
}
