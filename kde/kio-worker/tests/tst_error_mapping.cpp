// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for backend-failure -> KIO classification.
//
// Pure logic: a kind and a message in, a code and a message out. No D-Bus, no
// process, no image, so this runs everywhere including CI.
//
// The mapping is what a user actually sees, and a wrong code is not cosmetic: it
// decides what Dolphin shows and which actions it offers. Two properties are
// asserted throughout -- every failure gets a NON-EMPTY message, and every message
// NAMES the artifact, because "could not be read" with no subject is
// indistinguishable from a bug in the file manager.

#include "error_mapping.h"

#include <QTest>

using namespace DwarfsKio;

namespace {

const QString kArtifact = QStringLiteral("dwarfs:///home/u/img.dwarfs/sub");

ClassifiedFailure classify(const QString &kind, const QString &message = QString())
{
    return classifyFailure(BackendFailure{kind, message}, kArtifact);
}

} // namespace

class ErrorMappingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- each kind maps to its intended code --------------------------------

    void invalidPathIsDoesNotExist()
    {
        // The path or image is absent. Distinct from "unreadable": the user's next
        // step differs (check the path vs check permissions).
        QCOMPARE(classify(QStringLiteral("InvalidPath")).kioCode, int(KIO::ERR_DOES_NOT_EXIST));
    }

    void dependencyMissingIsCannotLaunch()
    {
        // Not the URL's fault, and retrying will not help, so it must not look like
        // an access error.
        QCOMPARE(classify(QStringLiteral("DependencyMissing")).kioCode,
                 int(KIO::ERR_CANNOT_LAUNCH_PROCESS));
    }

    void lockBusyIsServiceNotAvailable()
    {
        // Another operation holds the image: transient by nature. KIO has no
        // ERR_BUSY, so ERR_SERVICE_NOT_AVAILABLE is used -- it reads as "not right
        // now" rather than as a permission problem the user cannot fix.
        QCOMPARE(classify(QStringLiteral("LockBusy")).kioCode,
                 int(KIO::ERR_SERVICE_NOT_AVAILABLE));
    }

    void mountStateErrorIsInternal()
    {
        // Mount state belongs to the tray. If it reaches this component something
        // is wrong upstream, so it must not masquerade as an access error.
        QCOMPARE(classify(QStringLiteral("MountStateError")).kioCode, int(KIO::ERR_INTERNAL));
    }

    void badCliUsageIsInternal()
    {
        // Means the worker built an argv dwarfsctl rejected: our bug, and it should
        // read as one rather than as something the user did.
        QCOMPARE(classify(QStringLiteral("BadCliUsage")).kioCode, int(KIO::ERR_INTERNAL));
    }

    void timeoutIsServerTimeout()
    {
        QCOMPARE(classify(QStringLiteral("Timeout")).kioCode, int(KIO::ERR_SERVER_TIMEOUT));
    }

    void unparseableOutputIsInternalNotAnEmptyListing()
    {
        // The failure this project has already shipped once: a listing that could
        // not be read rendering as a valid empty directory. It must be an error.
        for (const char *kind : {"EmptyOutput", "MalformedEnvelope"}) {
            const ClassifiedFailure c = classify(QLatin1String(kind));
            QCOMPARE(c.kioCode, int(KIO::ERR_INTERNAL));
            QVERIFY2(!c.message.isEmpty(), kind);
        }
    }

    void anUnknownKindIsStillAnError()
    {
        // A backend that grows a new kind must not produce a success or a crash.
        for (const char *kind : {"SomethingNew", "", "internal"}) {
            const ClassifiedFailure c = classify(QLatin1String(kind));
            QCOMPARE(c.kioCode, int(KIO::ERR_INTERNAL));
            QVERIFY2(!c.message.isEmpty(), kind);
        }
    }

    // --- every failure explains itself ---------------------------------------

    void everyKindProducesANonEmptyMessage()
    {
        const QStringList kinds{
            QStringLiteral("InvalidPath"),  QStringLiteral("DependencyMissing"),
            QStringLiteral("LockBusy"),     QStringLiteral("MountStateError"),
            QStringLiteral("BadCliUsage"),  QStringLiteral("EmptyOutput"),
            QStringLiteral("MalformedEnvelope"), QStringLiteral("Timeout"),
            QStringLiteral("Unknown"),      QString(),
        };
        for (const QString &kind : kinds) {
            const ClassifiedFailure c = classify(kind);
            QVERIFY2(!c.message.isEmpty(), qPrintable(QStringLiteral("kind=%1").arg(kind)));
        }
    }

    void messagesNameTheArtifactWhenTheBackendDoesNot()
    {
        // With no backend message, the classification must still say WHICH thing
        // failed. A subjectless error is unactionable.
        const ClassifiedFailure c = classify(QStringLiteral("InvalidPath"));
        QVERIFY2(c.message.contains(kArtifact), qPrintable(c.message));
    }

    void theBackendsOwnMessageIsPreserved()
    {
        // The backend names the file and the cause ("X is not a DwarFS image
        // (dwarfsck: no filesystem found)"). Replacing that with something generic
        // here would throw away the only specific information available.
        const QString backend = QStringLiteral(
            "/tmp/notes.txt is not a DwarFS image (dwarfsck: no filesystem found)");
        const ClassifiedFailure c = classify(QStringLiteral("InvalidPath"), backend);
        QCOMPARE(c.message, backend);
    }

    void anEmptyArtifactStillProducesAReadableMessage()
    {
        // Nothing to name, but the message must not become " does not exist".
        const ClassifiedFailure c = classifyFailure(
            BackendFailure{QStringLiteral("InvalidPath"), QString()}, QString());
        QVERIFY(!c.message.isEmpty());
        QVERIFY2(!c.message.startsWith(QLatin1Char(' ')), qPrintable(c.message));
        QVERIFY(!c.message.contains(QStringLiteral("%1")));
    }

    void classificationDependsOnlyOnKindNotOnMessageText()
    {
        // Two failures with the same kind but different wording must classify
        // identically, so a reworded backend message cannot change the code.
        const ClassifiedFailure a = classify(QStringLiteral("LockBusy"),
                                             QStringLiteral("image is locked"));
        const ClassifiedFailure b = classify(QStringLiteral("LockBusy"),
                                             QStringLiteral("something else entirely"));
        QCOMPARE(a.kioCode, b.kioCode);
        QCOMPARE(a.kioCode, int(KIO::ERR_SERVICE_NOT_AVAILABLE));
    }

    void aMisleadingMessageDoesNotChangeTheCode()
    {
        // Even if a message mentions "not found" while the kind says otherwise, the
        // KIND wins. This is what "map on kind, never on text" means in practice.
        const ClassifiedFailure c = classify(QStringLiteral("DependencyMissing"),
                                             QStringLiteral("file not found: dwarfsck"));
        QCOMPARE(c.kioCode, int(KIO::ERR_CANNOT_LAUNCH_PROCESS));
    }
};

QTEST_MAIN(ErrorMappingTest)
#include "tst_error_mapping.moc"
