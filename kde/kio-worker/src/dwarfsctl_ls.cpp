// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctl_ls.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>

namespace DwarfsKio {

namespace {

/// Resolve the backend program.
///
/// Mirrors DwarfsctlRunner::dwarfsctlProgram() in kde/kf6-plugin: DWARFSCTL_BIN
/// wins, otherwise "dwarfsctl" is looked up on PATH. Resolved through
/// QStandardPaths so a bare name is not handed to the process launcher blindly.
QString programPath()
{
    const QString override = qEnvironmentVariable("DWARFSCTL_BIN");
    if (!override.isEmpty()) {
        return override;
    }
    const QString found = QStandardPaths::findExecutable(QStringLiteral("dwarfsctl"));
    return found.isEmpty() ? QStringLiteral("dwarfsctl") : found;
}

/// Identity of an image file as far as the cache is concerned.
///
/// Size + mtime is the same freshness test `make` uses. It is not a cryptographic
/// guarantee, and does not need to be: this cache only exists so a browse of one
/// image is not O(entries) process spawns, and a rebuilt or replaced image has a
/// different size or mtime in every realistic case. A hit for an image modified
/// within the same second to the same size would serve a stale listing; that is
/// accepted over the alternative (hashing the whole file per browse, which costs
/// more than the listings it would save).
struct CacheKey {
    QString path;
    qint64 size = -1;
    qint64 mtimeMs = -1;

    bool operator==(const CacheKey &o) const
    {
        return path == o.path && size == o.size && mtimeMs == o.mtimeMs;
    }
};

size_t qHash(const CacheKey &k, size_t seed = 0)
{
    return qHashMulti(seed, k.path, k.size, k.mtimeMs);
}

/// One cached listing, with the entry paths indexed for lookup.
struct CachedListing {
    CacheKey key;
    QVector<ListedEntry> entries;
    /// path -> index into `entries`. Built once when the listing is cached, so a
    /// `stat()` after a `listDir()` is a hash lookup rather than a scan.
    QHash<QString, int> byPath;
};

QHash<CacheKey, CachedListing> &listingCache()
{
    static QHash<CacheKey, CachedListing> cache;
    return cache;
}

/// The cache key for an image path, or a key that can never match when the file
/// cannot be stat'ed (in which case not caching is the honest behaviour).
CacheKey keyFor(const QString &imagePath)
{
    CacheKey key;
    key.path = imagePath;
    const QFileInfo info(imagePath);
    if (info.exists()) {
        key.size = info.size();
        key.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    }
    return key;
}

CachedListing *cachedListingFor(const QString &imagePath)
{
    const CacheKey key = keyFor(imagePath);
    if (key.size < 0) {
        return nullptr;
    }
    auto it = listingCache().find(key);
    if (it == listingCache().end()) {
        return nullptr;
    }
    return &it.value();
}

void remember(const QString &imagePath, const QVector<ListedEntry> &entries)
{
    const CacheKey key = keyFor(imagePath);
    if (key.size < 0) {
        return; // unreadable: do not cache a listing we could not attribute
    }
    CachedListing cached;
    cached.key = key;
    cached.entries = entries;
    cached.byPath.reserve(entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        cached.byPath.insert(entries.at(i).path, i);
    }
    listingCache().insert(key, cached);
}

/// Parse the `entries` array of a successful envelope into `out`.
///
/// Shared by `listImage` and `lookupEntry` so the two can never disagree about
/// what a valid entry looks like.
bool parseEntries(const QJsonObject &envelope, ListResult *out)
{
    const QJsonValue details = envelope.value(QStringLiteral("details"));
    if (!details.isObject()) {
        out->errorKind = QStringLiteral("MalformedEnvelope");
        out->error = QStringLiteral("dwarfsctl envelope has no details object");
        return false;
    }

    const QJsonValue entriesValue = details.toObject().value(QStringLiteral("entries"));
    if (!entriesValue.isArray()) {
        out->errorKind = QStringLiteral("MalformedEnvelope");
        out->error = QStringLiteral("dwarfsctl envelope has no entries array");
        return false;
    }

    const QJsonArray entries = entriesValue.toArray();
    out->entries.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject obj = value.toObject();

        ListedEntry entry;
        entry.name = obj.value(QStringLiteral("name")).toString();
        entry.path = obj.value(QStringLiteral("path")).toString();
        entry.isDir = obj.value(QStringLiteral("is_dir")).toBool(false);

        // Absent and zero are different facts. A directory has no size; a
        // zero-byte file has a size of zero. Collapsing them would make an empty
        // file indistinguishable from a directory.
        const QJsonValue size = obj.value(QStringLiteral("size"));
        if (size.isDouble()) {
            entry.hasSize = true;
            entry.size = static_cast<quint64>(size.toDouble());
        }

        const QJsonValue mode = obj.value(QStringLiteral("mode"));
        if (mode.isDouble()) {
            entry.hasMode = true;
            entry.mode = static_cast<quint32>(mode.toDouble());
        }

        out->entries.append(entry);
    }

    out->ok = true;
    return true;
}

/// Run one argv-only dwarfsctl invocation and capture its streams.
///
/// The single place a backend process is launched from this module, so the
/// timeout, kill and exit-code handling exist once.
ListResult runDwarfsctl(const QStringList &args, int timeoutMs)
{
    ListResult out;

    const QString program = programPath();

    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.start();

    if (!process.waitForStarted(10000)) {
        out.errorKind = QStringLiteral("DependencyMissing");
        out.error = QStringLiteral("could not start %1: %2")
                        .arg(program, process.errorString());
        return out;
    }

    const bool finished = process.waitForFinished(timeoutMs);
    if (!finished) {
        process.kill();
        process.waitForFinished(5000);
        out.errorKind = QStringLiteral("Timeout");
        out.error = QStringLiteral("dwarfsctl did not finish within %1 ms").arg(timeoutMs);
        return out;
    }

    const QByteArray stdoutBytes = process.readAllStandardOutput();
    const QByteArray stderrBytes = process.readAllStandardError();
    const int exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;

    return parseEnvelope(stdoutBytes, exitCode, stderrBytes);
}

} // namespace

ListResult parseEnvelope(const QByteArray &stdoutBytes, int exitCode, const QByteArray &stderrBytes)
{
    ListResult out;

    const QByteArray trimmed = stdoutBytes.trimmed();

    // A non-zero exit is a failure regardless of what was printed. Checking this
    // FIRST matters: if the exit status were ignored and the (possibly empty)
    // stdout parsed instead, a broken backend would look like an image with no
    // contents. That is the fabricated-success class this project has shipped
    // twice -- `extract` reporting success while writing nothing, and `build`
    // accepting a zero-byte artifact.
    if (exitCode != 0) {
        out.errorKind = QStringLiteral("BackendFailed");
        out.error = QStringLiteral("dwarfsctl ls failed (exit code %1)").arg(exitCode);

        // Prefer the backend's own explanation when it gave one. The envelope is
        // JSON on stdout even for failures, so try that before falling back to
        // stderr, which is diagnostics rather than contract.
        const QJsonObject envelope = QJsonDocument::fromJson(trimmed).object();
        const QJsonObject error = envelope.value(QStringLiteral("error")).toObject();
        if (!error.isEmpty()) {
            out.errorKind = error.value(QStringLiteral("kind")).toString(out.errorKind);
            const QString message = error.value(QStringLiteral("message")).toString();
            if (!message.isEmpty()) {
                out.error = message;
            }
        }
        if (out.error.isEmpty()) {
            const QString err = QString::fromUtf8(stderrBytes).trimmed();
            if (!err.isEmpty()) {
                out.error = err;
            }
        }
        return out;
    }

    if (trimmed.isEmpty()) {
        out.errorKind = QStringLiteral("EmptyOutput");
        out.error = QStringLiteral("dwarfsctl ls exited 0 but produced no output; "
                                   "the listing could not be read");
        return out;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        out.errorKind = QStringLiteral("MalformedEnvelope");
        out.error = QStringLiteral("could not parse the dwarfsctl envelope: %1")
                        .arg(parseError.errorString());
        return out;
    }

    const QJsonObject envelope = doc.object();

    // `ok:false` with exit 0 would be contradictory; trust the flag and report the
    // backend's message rather than treating the payload as a listing.
    if (!envelope.value(QStringLiteral("ok")).toBool(false)) {
        const QJsonObject error = envelope.value(QStringLiteral("error")).toObject();
        out.errorKind = error.value(QStringLiteral("kind")).toString(QStringLiteral("BackendError"));
        out.error = error.value(QStringLiteral("message"))
                        .toString(QStringLiteral("dwarfsctl reported an error"));
        return out;
    }

    const QJsonValue details = envelope.value(QStringLiteral("details"));
    if (!details.isObject()) {
        // parseEntries reports the same condition for the lookup path; keep the
        // wording identical so callers see one contract, not two.
        out.errorKind = QStringLiteral("MalformedEnvelope");
        out.error = QStringLiteral("dwarfsctl envelope has no details object");
        return out;
    }

    if (!parseEntries(envelope, &out)) {
        return out;
    }

    return out;
}

ListResult listImage(const QString &imagePath, int timeoutMs)
{
    // A cached listing for an unchanged image is served outright: this is what
    // makes a directory browse one call instead of one call plus one per entry.
    if (const CachedListing *cached = cachedListingFor(imagePath)) {
        ListResult out;
        out.ok = true;
        out.entries = cached->entries;
        return out;
    }

    const ListResult out = runDwarfsctl({QStringLiteral("ls"), imagePath}, timeoutMs);
    if (out.ok) {
        remember(imagePath, out.entries);
    }
    return out;
}

ListResult lookupEntry(const QString &imagePath, const QString &innerPath, int timeoutMs)
{
    const QString want = innerPath.isEmpty() ? QStringLiteral("/") : innerPath;

    // Warm path: the full listing for an unchanged image is already in hand, so
    // resolving one entry costs nothing. Absence here is genuine absence: the
    // cached listing is the whole tree, so a miss is InvalidPath with no process
    // run rather than a fabricated empty success (or a wasted call to re-learn
    // what is already known).
    if (const CachedListing *cached = cachedListingFor(imagePath)) {
        const int index = cached->byPath.value(want, -1);
        ListResult out;
        if (index < 0) {
            out.errorKind = QStringLiteral("InvalidPath");
            out.error = QStringLiteral("no such path in image: %1").arg(want);
            return out;
        }
        out.ok = true;
        out.entries.append(cached->entries.at(index));
        return out;
    }

    // Cold path: narrow the listing to the one entry instead of the whole tree.
    return runDwarfsctl({QStringLiteral("ls"),
                         QStringLiteral("--path"),
                         want,
                         QStringLiteral("--entry-only"),
                         imagePath},
                        timeoutMs);
}

void clearListingCache()
{
    listingCache().clear();
}

QVector<ListedEntry> childrenOf(const QVector<ListedEntry> &entries, const QString &innerPath)
{
    const QString prefix = (innerPath == QLatin1String("/") || innerPath.isEmpty())
        ? QStringLiteral("/")
        : innerPath + QLatin1Char('/');

    QVector<ListedEntry> out;
    for (const ListedEntry &entry : entries) {
        if (entry.path == QLatin1String("/")) {
            continue; // the root is never its own child
        }
        if (!entry.path.startsWith(prefix)) {
            continue;
        }
        // Direct children only: nothing after the prefix may be a separator.
        if (entry.path.indexOf(QLatin1Char('/'), prefix.size()) != -1) {
            continue;
        }
        out.append(entry);
    }
    return out;
}

const ListedEntry *entryAt(const QVector<ListedEntry> &entries, const QString &innerPath)
{
    const QString want = innerPath.isEmpty() ? QStringLiteral("/") : innerPath;
    for (const ListedEntry &entry : entries) {
        if (entry.path == want) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace DwarfsKio
