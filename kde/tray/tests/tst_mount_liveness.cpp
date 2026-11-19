// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests for mount liveness: the rule that a mount table entry is not the same
// as a working mount.
//
// WHY THIS EXISTS. `mount()` used to treat "the mountpoint is listed in
// /proc/self/mountinfo" as proof the image was mounted, and returned success on
// that basis. A FUSE daemon that dies leaves its table entry behind, so the tray
// reported a healthy mount for a path that served nothing -- and `dwarfs` then
// refuses to mount over the leftover entry, leaving the state unrecoverable from
// the GUI.
//
// The decision logic is tested here as pure functions, so it runs on any machine
// including CI, which has no /dev/fuse. The end-to-end proof -- a real FUSE
// daemon killed, a real entry left behind -- is gated at the bottom of the file.

#include "mount_liveness.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTest>
#include <QTemporaryDir>
#include <QElapsedTimer>

#include <cerrno>
#include <csignal>
#include <future>
#include <vector>
#include <QSemaphore>
#include <memory>
#include <sys/types.h>
#include <sys/stat.h>

using namespace dwarfsctl_tray;

namespace {

bool integrationEnabled()
{
    return qEnvironmentVariable("DWARFSCTL_INTEGRATION") == QLatin1String("1");
}

QString findTool(const QString &name)
{
    return QStandardPaths::findExecutable(name);
}

} // namespace

class MountLivenessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --- the classification of a probe result ------------------------------

    /// A served request means the mount works. This is the only Live case.
    void statfsSuccessIsLive()
    {
        QCOMPARE(classifyStatfsResult(0, 0), MountLiveness::Live);
        // A stale errno value must not change a successful result.
        QCOMPARE(classifyStatfsResult(0, ENOTCONN), MountLiveness::Live);
    }

    /// ENOTCONN is the signature of a dead FUSE daemon.
    void statfsNotConnectedIsDead()
    {
        QCOMPARE(classifyStatfsResult(-1, ENOTCONN), MountLiveness::Dead);
    }

    /// Other errnos describe the path, not the daemon. Calling them Dead would
    /// hide a healthy mount behind a permissions or path problem.
    void otherErrorsAreNotDead()
    {
        for (int err : {ENOENT, EACCES, EPERM, EIO, ENOTDIR}) {
            QCOMPARE(classifyStatfsResult(-1, err), MountLiveness::Unknown);
        }
    }

    // --- the decision the fix rests on -------------------------------------

    /// The regression guard, stated as the invariant: being listed is not
    /// enough. If this ever passes with `listed=true, Dead` answering true, the
    /// original defect is back.
    void beingListedIsNotEnoughWhenTheMountIsDead()
    {
        QVERIFY2(!mountEntryIsUsable(true, MountLiveness::Dead),
                 "a listed-but-dead mount must not be treated as usable");
        QVERIFY2(!mountEntryIsUsable(true, MountLiveness::Unresponsive),
                 "a listed-but-wedged mount must not be treated as usable");
    }

    void aLiveListedMountIsUsable()
    {
        QVERIFY(mountEntryIsUsable(true, MountLiveness::Live));
    }

    /// A probe that could not run is no evidence of a broken mount. Treating
    /// Unknown as unusable would hide healthy mounts wherever fork is blocked.
    void anInconclusiveProbeKeepsTheMount()
    {
        QVERIFY(mountEntryIsUsable(true, MountLiveness::Unknown));
    }

    void anUnlistedMountpointIsNeverUsable()
    {
        for (auto liveness : {MountLiveness::Live,
                              MountLiveness::Dead,
                              MountLiveness::Unresponsive,
                              MountLiveness::Unknown}) {
            QVERIFY(!mountEntryIsUsable(false, liveness));
        }
    }

    // --- the probe itself, on real paths -----------------------------------

    /// A plain directory is not a mount, and the probe must not claim it is.
    void isMountedIsFalseForAPlainDirectory()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QVERIFY(!isMounted(tmp.path()));
        // A path that does not exist is not a mount either.
        QVERIFY(!isMounted(tmp.path() + QStringLiteral("/nope")));
    }

    /// The error class behind the mount-table bug, pinned as a test. procfs
    /// reports st_size == 0 for every file, and QFile::atEnd() decides end of
    /// data from that reported size -- so a reader gated on atEnd() reads
    /// nothing from /proc at all. Both halves are asserted: the zero size is
    /// real, and the data is there to be read anyway.
    void procfsReportsNoSizeButStillYieldsData()
    {
        const QString path = QStringLiteral("/proc/self/mountinfo");
        if (!QFileInfo::exists(path)) {
            qWarning() << "SKIP: /proc/self/mountinfo is not available";
            return;
        }

        QFile f(path);
        QVERIFY2(f.open(QIODevice::ReadOnly), "the mount table must be readable");
        QCOMPARE(f.size(), 0); // the trap: the metadata claims the file is empty
        QVERIFY2(!f.readAll().isEmpty(), "the same file yields data when the read decides the end");
    }

    /// A direct regression guard for the reader: "/" is always a mount, so a
    /// negative answer can only mean the mount table was never read. This is
    /// the assertion that failed before the reader was fixed -- every other
    /// test in this file passed while isMounted() was blind.
    void theRootMountIsAlwaysSeen()
    {
        QVERIFY2(isMounted(QStringLiteral("/")),
                 "the root filesystem is a mount; false here means the mount table was not read");
    }

    /// The probe answers for a real filesystem, and reports it as live: the
    /// bound and the fork must not turn a working path into a failure.
    void optionalMetadataReadsAreRegularAndBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto regular = directory.filePath(QStringLiteral("icon.json"));
        QFile file(regular); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray expected("{\"version\":1,\"places\":{\"icon\":\"folder\"}}");
        QCOMPARE(file.write(expected), expected.size()); file.close();
        QCOMPARE(boundedRegularFileRead(regular), expected);
        QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(QByteArray(4097, 'x')), qint64(4097)); file.close();
        QVERIFY(boundedRegularFileRead(regular).isEmpty());
        const auto fifo = directory.filePath(QStringLiteral("icon.fifo"));
        QVERIFY(::mkfifo(QFile::encodeName(fifo).constData(), 0600) == 0);
        QElapsedTimer timer; timer.start();
        QVERIFY(boundedRegularFileRead(fifo).isEmpty());
        QVERIFY2(timer.elapsed() < 1000, "Optional FIFO metadata blocked a reader");
    }

    void kernelObservationDistinguishesPresentAndAbsent()
    {
        QCOMPARE(observeMount(QStringLiteral("/proc")).presence, MountPresence::Present);
        QCOMPARE(observeMount(QStringLiteral("/proc")).source, QStringLiteral("proc"));
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QCOMPARE(observeMount(directory.path()).presence, MountPresence::Absent);
        QCOMPARE(observeMount(directory.filePath(QStringLiteral("absent"))).presence, MountPresence::Absent);
        QCOMPARE(observeMount({}).presence, MountPresence::Unknown);
    }

    void boundedMetadataPreservesIdentityAndMissingErrors()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        struct stat value {};
        int error = -1;
        QVERIFY(boundedFileStat(directory.path(), &value, &error));
        QCOMPARE(error, 0);
        QVERIFY(S_ISDIR(value.st_mode));
        QVERIFY(!boundedFileStat(directory.filePath(QStringLiteral("absent")), &value, &error));
        QCOMPARE(error, ENOENT);
    }

    void boundedCanonicalPathPreservesAliases()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto alias = directory.filePath(QStringLiteral("alias"));
        QVERIFY(QFile::link(directory.path(), alias));
        QCOMPARE(boundedCanonicalFilePath(alias), directory.path());
        QVERIFY(boundedCanonicalFilePath(directory.filePath(QStringLiteral("absent"))).isEmpty());
        QVERIFY(isMounted(QStringLiteral("/")));
    }

    void probeOnAWorkingFilesystemIsLive()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QCOMPARE(probeMountLiveness(tmp.path()), MountLiveness::Live);
    }

    void kernelSourceMustMatchTheRequestedImage()
    {
        // procfs has a real, known kernel mount source. This exercises the
        // parser without introducing a mount or relying on a registry claim.
        if (!isMounted(QStringLiteral("/proc"))) QSKIP("procfs mount unavailable");
        QVERIFY(mountImageMatches(QStringLiteral("/proc"), QStringLiteral("proc")));
        QVERIFY(!mountImageMatches(QStringLiteral("/proc"), QStringLiteral("/missing/image.dwarfs")));
        QTemporaryDir plain;
        QVERIFY(!mountImageMatches(plain.path(), QStringLiteral("proc")));
    }

    void concurrentProbesKeepTheirOwnChildrenAndLeaveQProcessAlone()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        // A separate QProcess child must remain available to its owning
        // QProcess while many threads simultaneously create/reap probe children.
        QProcess unrelated;
        unrelated.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 0.1; exit 37")});
        QVERIFY(unrelated.waitForStarted(3000));
        const auto gate = std::make_shared<QSemaphore>();
        std::vector<std::future<bool>> results;
        for (int thread = 0; thread < 8; ++thread) {
            results.push_back(std::async(std::launch::async, [gate, path = tmp.path()] {
                gate->acquire();
                for (int call = 0; call < 8; ++call)
                    if (probeMountLiveness(path) != MountLiveness::Live) return false;
                return true;
            }));
        }
        gate->release(8);
        // Collect every future before reporting failures, retaining ownership of
        // every worker even if one probe reports an unexpected result.
        bool live = true;
        for (auto &result : results) live = result.get() && live;
        QVERIFY(live);
        QVERIFY(unrelated.waitForFinished(3000));
        QCOMPARE(unrelated.exitStatus(), QProcess::NormalExit);
        QCOMPARE(unrelated.exitCode(), 37);
    }

    /// A nonexistent path yields Unknown rather than Dead: the daemon (there is
    /// none) is not what is wrong, and the caller must not read "Dead" into it.
    void probeOnAMissingPathIsNotDead()
    {
        const auto r = probeMountLiveness(QStringLiteral("/nonexistent/hermes/probe"));
        QVERIFY2(r != MountLiveness::Dead, "a missing path must not be reported as a dead mount");
    }

    /// mountLivenessName covers every enumerator; the JSON detail depends on it.
    void namesAreStable()
    {
        QCOMPARE(mountLivenessName(MountLiveness::Live), QStringLiteral("live"));
        QCOMPARE(mountLivenessName(MountLiveness::Dead), QStringLiteral("dead"));
        QCOMPARE(mountLivenessName(MountLiveness::Unresponsive), QStringLiteral("unresponsive"));
        QCOMPARE(mountLivenessName(MountLiveness::Unknown), QStringLiteral("unknown"));
    }

    // --- end-to-end: a real dead FUSE mount --------------------------------
    //
    // This is the only test that reproduces the actual reported failure. It needs
    // a real FUSE mount and the `dwarfs` tool, so it is gated on
    // DWARFSCTL_INTEGRATION=1 and prints a loud SKIP otherwise -- a silent skip
    // reads as a pass, which is how a suite rots without anyone noticing.

    /// The heart of the finding, reproduced: kill the daemon, and the mountpoint
    /// is STILL listed in the mount table while serving nothing. Before the fix
    /// that combination was reported as a successful mount.
    void aDeadFuseMountIsListedButNotUsable()
    {
        if (!integrationEnabled()) {
            qWarning() << "SKIP: needs DWARFSCTL_INTEGRATION=1 (real FUSE mount)";
            return;
        }
        if (findTool(QStringLiteral("mkdwarfs")).isEmpty()
            || findTool(QStringLiteral("dwarfs")).isEmpty()) {
            qWarning() << "SKIP: mkdwarfs/dwarfs not on PATH";
            return;
        }
        if (!QFileInfo::exists(QStringLiteral("/dev/fuse"))) {
            qWarning() << "SKIP: /dev/fuse is not available";
            return;
        }

        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());

        // Build a small image.
        const QString src = tmp.filePath(QStringLiteral("src"));
        QVERIFY(QDir().mkpath(src));
        {
            QFile f(QDir(src).filePath(QStringLiteral("hello.txt")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("hello\n");
        }
        const QString image = tmp.filePath(QStringLiteral("fixture.dwarfs"));
        {
            QProcess mk;
            mk.setProgram(findTool(QStringLiteral("mkdwarfs")));
            mk.setArguments({QStringLiteral("-i"), src, QStringLiteral("-o"), image,
                             QStringLiteral("-l4"), QStringLiteral("--no-progress")});
            mk.start();
            QVERIFY(mk.waitForFinished(300000));
            QCOMPARE(mk.exitCode(), 0);
        }

        const QString mp = tmp.filePath(QStringLiteral("mnt"));
        QVERIFY(QDir().mkpath(mp));

        // Mount in the foreground so the daemon is a child we can kill. The
        // daemonising default is what makes this test wrong if used: killing the
        // launcher would leave the real daemon running and the mount healthy.
        QProcess dwarfs;
        dwarfs.setProgram(findTool(QStringLiteral("dwarfs")));
        dwarfs.setArguments({QStringLiteral("-f"), image, mp});
        // Merge channels so a daemon that refuses to mount explains itself in
        // the failure message below rather than failing silently.
        dwarfs.setProcessChannelMode(QProcess::MergedChannels);
        dwarfs.start();
        QVERIFY(dwarfs.waitForStarted(10000));

        // Wait for the mount to appear.
        bool appeared = false;
        for (int i = 0; i < 300 && !appeared; ++i) {
            appeared = isMounted(mp);
            if (!appeared)
                QTest::qWait(50);
        }
        if (!appeared) {
            dwarfs.kill();
            dwarfs.waitForFinished(5000);
            const QString daemonOutput = QString::fromUtf8(dwarfs.readAll());
            QFAIL(qPrintable(QStringLiteral("the fixture mount did not appear; dwarfs said: %1")
                                 .arg(daemonOutput.isEmpty() ? QStringLiteral("(nothing)")
                                                             : daemonOutput.trimmed())));
        }

        // A live mount is live, and usable.
        QCOMPARE(probeMountLiveness(mp), MountLiveness::Live);
        QVERIFY2(mountsUsable(mp), "a live mount must be usable");

        // Kill the daemon (SIGKILL: no cleanup, exactly what a crash looks like).
        ::kill(static_cast<pid_t>(dwarfs.processId()), SIGKILL);
        QVERIFY(dwarfs.waitForFinished(10000));

        // THE FINDING: the entry is still listed, but nothing can be read.
        QVERIFY2(isMounted(mp), "the mount table entry is expected to outlive the daemon");
        QCOMPARE(probeMountLiveness(mp), MountLiveness::Dead);
        QVERIFY2(!mountsUsable(mp), "a dead mount must not be usable");

        // Leave the machine as it was found. A dead entry is litter -- exactly
        // the state this fix exists to clear -- and QTemporaryDir cannot remove
        // the directory while it is a mountpoint. Best-effort: the assertions
        // above are the test, this is hygiene.
        for (const auto& tool : {QStringLiteral("fusermount3"), QStringLiteral("fusermount")}) {
            const QString toolPath = findTool(tool);
            if (toolPath.isEmpty()) {
                continue;
            }
            (void)QProcess::execute(toolPath, {QStringLiteral("-u"), mp});
            break;
        }
    }
};

QTEST_MAIN(MountLivenessTest)
#include "tst_mount_liveness.moc"
