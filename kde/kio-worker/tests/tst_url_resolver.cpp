// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for the dwarfs: URL split. These run with no KIO and no DwarFS tools
// installed, because the rules here are pure logic and the failure modes that
// matter (traversal, ambiguity, encoding) are all decidable without I/O.

#include "url_resolver.h"

#include <QTest>

using namespace DwarfsKio;

class UrlResolverTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- the happy paths ---------------------------------------------------

    void splitsImageFromInnerPath()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs/sub/file.txt")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/img.dwarfs"));
        QCOMPARE(r.innerPath, QStringLiteral("/sub/file.txt"));
    }

    void rootOfImageIsTheImageItself()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/img.dwarfs"));
        QCOMPARE(r.innerPath, QStringLiteral("/"));
    }

    void trailingSlashIsTheImageRootToo()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs/")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/img.dwarfs"));
        QCOMPARE(r.innerPath, QStringLiteral("/"));
    }

    // The image need not be at the filesystem root; directories before it are
    // part of the image path, not the inner path.
    void handlesImageInANestedDirectory()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/Images/a/b.dwarfs/x.txt")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/Images/a/b.dwarfs"));
        QCOMPARE(r.innerPath, QStringLiteral("/x.txt"));
    }

    // --- rejection: shape --------------------------------------------------

    void rejectsNonDwarfsScheme()
    {
        const auto r = resolve(QUrl(QStringLiteral("file:///home/u/img.dwarfs")));
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void rejectsMissingImage()
    {
        // No component ends in .dwarfs, so there is no image to open.
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/notes.txt/x")));
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains(QStringLiteral(".dwarfs")));
    }

    void rejectsARelativeImagePath()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:relative/img.dwarfs")));
        QVERIFY(!r.ok);
    }

    void rejectsHostComponent()
    {
        // dwarfs://host/path is ambiguous; only dwarfs:///abs/path is supported.
        const auto r = resolve(QUrl(QStringLiteral("dwarfs://host/img.dwarfs/x")));
        QVERIFY(!r.ok);
    }

    // --- rejection: traversal (the security-relevant cases) ----------------

    void rejectsTraversalInInnerPath()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs/../../etc/passwd")));
        QVERIFY2(!r.ok, "traversal in the inner path must be rejected");
    }

    void rejectsTraversalBeforeTheImage()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/../etc/img.dwarfs")));
        QVERIFY(!r.ok);
    }

    // A "." component is rejected rather than normalised away: normalising would
    // silently make the URL address a different file than it names.
    void rejectsDotComponentInInnerPath()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs/a/./b")));
        QVERIFY(!r.ok);
    }

    // --- encoding ----------------------------------------------------------

    // A mountpoint or filename may contain spaces or non-ASCII. The Rust backend
    // shipped a bug of exactly this class, so it is covered explicitly.
    void roundTripsEncodedPaths()
    {
        const auto r = resolve(
            QUrl(QStringLiteral("dwarfs:///home/u/my%20images/caf%C3%A9.dwarfs/a%20b.txt")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/my images/café.dwarfs"));
        QCOMPARE(r.innerPath, QStringLiteral("/a b.txt"));
    }

    void toUrlEncodesWhatResolveDecodes()
    {
        const QString image = QStringLiteral("/home/u/my images/café.dwarfs");
        const QString inner = QStringLiteral("/a b.txt");
        const QUrl url = toUrl(image, inner);

        // Assert against the ENCODED form, not toString(). toString() defaults to
        // PrettyDecoded, which deliberately shows spaces and non-ASCII in
        // human-readable form (verified: it prints "my images/café.dwarfs" with
        // the raw characters even though the URL is correctly encoded). The
        // encoded form is what a URL consumer actually receives.
        const QByteArray encoded = url.toEncoded();
        QVERIFY(!encoded.contains(' '));
        // QT_NO_CAST_FROM_ASCII is set by KDECompilerSettings, so compare against
        // QByteArray/QString explicitly rather than relying on implicit char* casts.
        QVERIFY(encoded.contains(QByteArrayLiteral("%20")));
        QVERIFY(url.toString(QUrl::FullyEncoded).contains(QLatin1String("%C3%A9")));
        QVERIFY(url.isValid());

        // And the round trip through resolve() is lossless.
        const auto back = resolve(url);
        QVERIFY2(back.ok, qPrintable(back.error));
        QCOMPARE(back.imagePath, image);
        QCOMPARE(back.innerPath, inner);
    }

    void toUrlOfImageRootHasNoTrailingComponent()
    {
        const QUrl url = toUrl(QStringLiteral("/home/u/img.dwarfs"), QStringLiteral("/"));
        const auto back = resolve(url);
        QVERIFY2(back.ok, qPrintable(back.error));
        QCOMPARE(back.imagePath, QStringLiteral("/home/u/img.dwarfs"));
        QCOMPARE(back.innerPath, QStringLiteral("/"));
    }

    // A component containing a literal '/' cannot exist after splitting, but a
    // component that looks like a nested path must not be treated as one.
    void innerPathWithDoubledSlashesCollapses()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/img.dwarfs/a//b")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.innerPath, QStringLiteral("/a/b"));
    }

    // The image suffix match is case-insensitive: a file named IMG.DWARFS is
    // still a DwarFS image, and the MIME type treats it that way.
    void acceptsUppercaseImageSuffix()
    {
        const auto r = resolve(QUrl(QStringLiteral("dwarfs:///home/u/IMG.DWARFS/x")));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imagePath, QStringLiteral("/home/u/IMG.DWARFS"));
    }
};

QTEST_MAIN(UrlResolverTest)
#include "tst_url_resolver.moc"
