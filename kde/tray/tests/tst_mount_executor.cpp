// SPDX-License-Identifier: GPL-3.0-or-later
#include "mount_executor.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <csignal>
#include <memory>
#include <future>
#include <chrono>
#include <unistd.h>

namespace {
QByteArray processStart(qint64 pid)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) return {};
    const auto raw = stat.readAll();
    const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
    if (fields.size() <= 19 || fields[0] == "Z") return {};
    return fields[19];
}
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("fixture write failed");
}
QByteArray contents(const QString &path)
{
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

class MountExecutorTest final : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> home;
    QString image, registry, marker;
    QByteArray oldHome, oldRuntime, oldConfig, oldEngine, oldPath;
private Q_SLOTS:
    void initTestCase()
    {
        oldHome = qgetenv("HOME"); oldRuntime = qgetenv("XDG_RUNTIME_DIR");
        oldConfig = qgetenv("XDG_CONFIG_HOME"); oldEngine = qgetenv("DWARFSCTL_ENGINE");
        oldPath = qgetenv("PATH");
    }
    void init()
    {
        home = std::make_unique<QTemporaryDir>(); QVERIFY(home->isValid());
        const QString runtime = home->filePath("runtime");
        QVERIFY(QDir().mkpath(runtime + "/dwarfsctl"));
        QVERIFY(QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        qputenv("HOME", home->path().toUtf8()); qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("XDG_CONFIG_HOME", home->filePath("config").toUtf8());
        qputenv("DWARFSCTL_ENGINE", QCoreApplication::applicationFilePath().toUtf8());
        image = home->filePath("fixture.dwarfs");
        registry = runtime + "/dwarfsctl/mounts.json";
        marker = home->filePath("tool.json"); qputenv("MOUNT_EXECUTOR_TEST_MARKER", marker.toUtf8());
        qunsetenv("MOUNT_EXECUTOR_TEST_HOLD");
        qunsetenv("MOUNT_EXECUTOR_TEST_CRASH");
        qputenv("PATH", oldPath);
        writeFile(image, "ordinary test fixture; tool helper never mounts\n");
    }
    void corruptRegistryRefusesLauncherAndPreservesBytes()
    {
        const QByteArray original("not valid JSON\n"); writeFile(registry, original);
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        QCOMPARE(result.value("error").toObject().value("kind").toString(), QString("Registry"));
        QVERIFY(!QFileInfo::exists(marker)); QCOMPARE(contents(registry), original);
    }
    void unwriteableRegistryRefusesLauncher()
    {
        QVERIFY(QDir().mkpath(registry));
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false)); QVERIFY(!QFileInfo::exists(marker));
        QVERIFY(QFileInfo(registry).isDir());
    }
    void lockedRegistryRefusesLauncherAndPreservesBytes()
    {
        const QByteArray original("{\"mounts\":[],\"keep\":true}\n"); writeFile(registry, original);
        QLockFile lock(QFileInfo(registry).dir().filePath("mounts.lock")); QVERIFY(lock.tryLock());
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false)); QVERIFY(!QFileInfo::exists(marker));
        QCOMPARE(contents(registry), original);
    }
    void ordinaryFailureRemovesOnlyItsIntent()
    {
        const QJsonArray others{QJsonObject{{"image", "/other/image.dwarfs"}, {"mountpoint", "/other/mount"}}};
        writeFile(registry, QJsonDocument(QJsonObject{{"mounts", others}, {"keep", true}}).toJson());
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false)); QVERIFY(QFileInfo::exists(marker));
        const auto after = QJsonDocument::fromJson(contents(registry)).object();
        QCOMPARE(after.value("mounts").toArray(), others); QCOMPARE(after.value("keep"), QJsonValue(true));
    }
    void longestImageFilenameUsesBoundedFullIdentityMountpoint()
    {
        image = home->filePath(QString(248, QLatin1Char('X')) + QStringLiteral(".dwarfs"));
        writeFile(image, "ordinary helper fixture");
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false)); // Helper deliberately refuses mounting.
        const auto point = QJsonDocument::fromJson(contents(marker)).object().value("mountpoint").toString();
        const auto name = QFileInfo(point).fileName();
        QVERIFY(!name.isEmpty()); QVERIFY(name.toUtf8().size() <= 255);
        const auto hash = QCryptographicHash::hash(QFileInfo(image).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
        QVERIFY(name.endsWith(QString::fromLatin1(hash)));
    }
    void authorityExitStopsOnlyItsTransientLauncher()
    {
        QProcess authority;
        auto env = QProcessEnvironment::systemEnvironment(); env.insert("MOUNT_EXECUTOR_TEST_HOLD", "1");
        authority.setProcessEnvironment(env);
        qint64 tool = 0; QByteArray started;
        const auto cleanup = qScopeGuard([&] {
            if (authority.state() != QProcess::NotRunning) { authority.kill(); authority.waitForFinished(2000); }
            if (tool > 0 && !started.isEmpty() && processStart(tool) == started) ::kill(pid_t(tool), SIGKILL);
        });
        authority.start(QCoreApplication::applicationFilePath(), {"--invoke-mount", image});
        QVERIFY(authority.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker), 5000);
        const auto actual = QJsonDocument::fromJson(contents(marker)).object();
        tool = actual.value("pid").toInteger(); started = actual.value("start").toString().toLatin1();
        QVERIFY(tool > 0 && !started.isEmpty()); QCOMPARE(processStart(tool), started);
        authority.terminate(); QVERIFY(authority.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(processStart(tool) != started, 2000);
    }
    void crashedLauncherIntentIsNotReportedMountedOrErased()
    {
        qputenv("MOUNT_EXECUTOR_TEST_CRASH", "1");
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).mount(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        const auto pending = QJsonDocument::fromJson(contents(registry)).object().value("mounts").toArray();
        QCOMPARE(pending.size(), 1);
        const auto generation = pending.first().toObject().value("source_generation").toObject();
        QCOMPARE(generation.size(), 7);
        for (const auto &field : generation) QVERIFY(field.isString() && !field.toString().isEmpty());
        QCOMPARE(pending.first().toObject().value("state").toString(), QString("pending"));
        const auto listed = QJsonDocument::fromJson(MountExecutor(nullptr).listMounts()).object();
        QCOMPARE(listed.value("ok"), QJsonValue(true));
        QCOMPARE(listed.value("details").toObject().value("count").toInt(), 0);
        QCOMPARE(QJsonDocument::fromJson(contents(registry)).object().value("mounts").toArray(), pending);
    }
    void unmountCannotEraseAnotherPendingLaunch()
    {
        const auto point = home->filePath("runtime/dwarfs/pending-volume");
        const QJsonObject record{{"image", image}, {"mountpoint", point}, {"state", "pending"}, {"operation", "owned-launch"}};
        const auto original = QJsonDocument(QJsonObject{{"mounts", QJsonArray{record}}}).toJson();
        writeFile(registry, original);
        const auto lockDir = home->filePath("runtime/dwarfs/locks"); QVERIFY(QDir().mkpath(lockDir));
        const auto hash = QCryptographicHash::hash(QFileInfo(image).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
        QLockFile launch(QDir(lockDir).filePath(QString::fromLatin1(hash) + ".lock")); QVERIFY(launch.tryLock());
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).unmountByMountpoint(point)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        QCOMPARE(result.value("error").toObject().value("kind").toString(), QString("Busy"));
        QCOMPARE(contents(registry), original); QVERIFY(!QFileInfo::exists(marker));
    }
    void missingSourceUsesRecordedMountpoint()
    {
        const auto absent = home->filePath("missing.dwarfs");
        const auto point = home->filePath("runtime/dwarfs/recorded-volume");
        QVERIFY(QDir().mkpath(point));
        writeFile(registry, QJsonDocument(QJsonObject{{"mounts", QJsonArray{
            QJsonObject{{"image", absent}, {"mountpoint", point}}}}}).toJson());
        const auto tools = home->filePath("tools"); QVERIFY(QDir().mkpath(tools));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), tools + "/fusermount3"));
        qputenv("PATH", tools.toUtf8());
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).unmountByImage(absent)).object();
        QCOMPARE(result.value("ok"), QJsonValue(true));
        QCOMPARE(result.value("details").toObject().value("mountpoint").toString(), point);
        QCOMPARE(QJsonDocument::fromJson(contents(marker)).object().value("mountpoint").toString(), point);
    }
    void imageSelectorCannotBorrowAnotherRecordsDerivedMountpoint()
    {
        const auto tools = home->filePath("selector-tools"); QVERIFY(QDir().mkpath(tools));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), tools + "/fusermount3"));
        qputenv("PATH", tools.toUtf8());
        const auto hash = QCryptographicHash::hash(QFileInfo(image).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
        const auto point = home->filePath("runtime/dwarfs/fixture-") + QString::fromLatin1(hash);
        const auto original = QJsonDocument(QJsonObject{{"mounts", QJsonArray{
            QJsonObject{{"image", home->filePath("another-image.dwarfs")}, {"mountpoint", point}}}}}).toJson();
        writeFile(registry, original);
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).unmountByImage(image)).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        QCOMPARE(contents(registry), original);
        QVERIFY(!QFileInfo::exists(marker));
    }
    void sourceAliasWithParentComponentDoesNotSelectLexicalNeighbour()
    {
        const auto tools = home->filePath("physical-tools"); QVERIFY(QDir().mkpath(tools));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), tools + "/fusermount3"));
        qputenv("PATH", tools.toUtf8());
        QVERIFY(QDir().mkpath(home->filePath("real/deep")));
        QVERIFY(QFile::link(home->filePath("real/deep"), home->filePath("alias-dir")));
        const auto physical = home->filePath("real/selected.dwarfs"); writeFile(physical, "physical source");
        const auto lexical = home->filePath("selected.dwarfs"); writeFile(lexical, "different source");
        const auto original = QJsonDocument(QJsonObject{{"mounts", QJsonArray{
            QJsonObject{{"image", lexical}, {"mountpoint", home->filePath("neighbour-point")}}}}}).toJson();
        writeFile(registry, original);
        const auto result = QJsonDocument::fromJson(MountExecutor(nullptr).unmountByImage(home->filePath("alias-dir/../selected.dwarfs"))).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        QCOMPARE(contents(registry), original);
        QVERIFY(!QFileInfo::exists(marker));
    }
    void changedRegistryRowCannotAuthorizeAnUnmount()
    {
        const QString point = home->filePath("recorded-point");
        const auto row = QJsonObject{{"image", image}, {"mountpoint", point}, {"mounted_at", 1}};
        writeFile(registry, QJsonDocument(QJsonObject{{"mounts", QJsonArray{row}}}).toJson());
        const auto locks = home->filePath("runtime/dwarfs/locks"); QVERIFY(QDir().mkpath(locks));
        const auto key = QCryptographicHash::hash(image.toUtf8(), QCryptographicHash::Sha256).toHex();
        QLockFile authority(locks + "/" + QString::fromLatin1(key) + ".lock");
        QVERIFY(authority.tryLock());
        auto result = std::async(std::launch::async, [point] { return MountExecutor(nullptr).unmountByMountpoint(point); });
        const auto blocked = result.wait_for(std::chrono::milliseconds(200));
        auto replacement = row; replacement.insert("mounted_at", 2);
        const auto original = QJsonDocument(QJsonObject{{"mounts", QJsonArray{replacement}}}).toJson();
        writeFile(registry, original);
        authority.unlock();
        const auto response = QJsonDocument::fromJson(result.get()).object();
        QCOMPARE(blocked, std::future_status::timeout);
        QCOMPARE(response.value("ok"), QJsonValue(false));
        QCOMPARE(response.value("error").toObject().value("kind").toString(), QString("MountStateError"));
        QVERIFY(!QFileInfo::exists(marker));
        QCOMPARE(contents(registry), original);
    }
    void foreignMountRefusesEveryUnmountSelector_data()
    {
        QTest::addColumn<QString>("selector");
        for (const auto &selector : {"point", "image", "all", "last"})
            QTest::newRow(selector) << QString::fromLatin1(selector);
    }
    void foreignMountRefusesEveryUnmountSelector()
    {
        QFETCH(QString, selector);
        // The root is a real listed mount whose source cannot be this fixture.
        // Helpers only record invocation and never perform an unmount.
        const QByteArray original = QJsonDocument(QJsonObject{{"mounts", QJsonArray{
            QJsonObject{{"image", image}, {"mountpoint", "/"}, {"mounted_at", 1}}}}}).toJson();
        writeFile(registry, original);
        const auto tools = home->filePath("tools"); QVERIFY(QDir().mkpath(tools));
        QVERIFY(QFile::link(QCoreApplication::applicationFilePath(), tools + "/fusermount3"));
        qputenv("PATH", tools.toUtf8());
        MountExecutor executor(nullptr);
        const auto raw = selector == "point" ? executor.unmountByMountpoint("/")
            : selector == "image" ? executor.unmountByImage(image)
            : selector == "all" ? executor.unmountAll() : executor.unmountLast();
        const auto result = QJsonDocument::fromJson(raw).object();
        QCOMPARE(result.value("ok"), QJsonValue(false));
        QVERIFY(!QFileInfo::exists(marker));
        QCOMPARE(contents(registry), original);
    }
    void cleanupTestCase()
    {
        const auto restore = [](const char *name, const QByteArray &value) {
            if (value.isNull()) qunsetenv(name); else qputenv(name, value);
        };
        restore("HOME", oldHome); restore("XDG_RUNTIME_DIR", oldRuntime);
        restore("XDG_CONFIG_HOME", oldConfig); restore("DWARFSCTL_ENGINE", oldEngine);
        restore("PATH", oldPath);
        qunsetenv("MOUNT_EXECUTOR_TEST_MARKER"); qunsetenv("MOUNT_EXECUTOR_TEST_HOLD");
        qunsetenv("MOUNT_EXECUTOR_TEST_CRASH");
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "dwarfs") {
        const auto marker = qEnvironmentVariable("MOUNT_EXECUTOR_TEST_MARKER");
        if (marker.isEmpty()) return 125;
        writeFile(marker, QJsonDocument(QJsonObject{{"pid", qint64(getpid())},
            {"start", QString::fromLatin1(processStart(getpid()))}, {"mountpoint", app.arguments().value(3)}}).toJson());
        if (qEnvironmentVariableIntValue("MOUNT_EXECUTOR_TEST_CRASH")) ::raise(SIGKILL);
        if (qEnvironmentVariableIntValue("MOUNT_EXECUTOR_TEST_HOLD")) QThread::sleep(15);
        return 73; // No mount syscall or external engine is used by this helper.
    }
    if (app.arguments().value(1) == "-u") {
        const auto marker = qEnvironmentVariable("MOUNT_EXECUTOR_TEST_MARKER");
        if (marker.isEmpty()) return 125;
        writeFile(marker, QJsonDocument(QJsonObject{{"mountpoint", app.arguments().value(2)}}).toJson());
        return 1; // Observe only the requested path; never unmount anything.
    }
    if (app.arguments().value(1) == "--invoke-mount") {
        MountExecutor executor(nullptr); executor.mount(app.arguments().value(2)); return 0;
    }
    MountExecutorTest test; return QTest::qExec(&test, argc, argv);
}
#include "tst_mount_executor.moc"
