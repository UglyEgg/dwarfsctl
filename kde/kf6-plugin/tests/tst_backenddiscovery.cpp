// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dwarfsctl_exec.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>
#include <cstdio>

class BackendDiscoveryTest final : public QObject {
    Q_OBJECT
    QString executable(const QString &path, bool enabled = true)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write("#!/bin/sh\nexit 0\n"); file.close();
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | (enabled ? QFileDevice::ExeOwner : QFileDevice::Permissions{}))) return {};
        return path;
    }
    QString resolve(const QString &root, const QString &override, const QString &path)
    {
        const QString child = root + QStringLiteral("/app/resolve");
        QDir().mkpath(QFileInfo(child).absolutePath());
        if (!QFileInfo::exists(child) && !QFile::copy(QCoreApplication::applicationFilePath(), child)) return {};
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("HOME"), root + QStringLiteral("/home"));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), root + QStringLiteral("/config"));
        environment.insert(QStringLiteral("XDG_DATA_HOME"), root + QStringLiteral("/data"));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), root + QStringLiteral("/cache"));
        environment.insert(QStringLiteral("PATH"), path);
        environment.insert(QStringLiteral("DWARFSCTL_BIN"), override);
        environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=/nonexistent/discovery-test"));
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(root);
        process.start(child, {QStringLiteral("--resolve")});
        if (!process.waitForFinished(5000) || process.exitCode() != 0) return {};
        return QString::fromUtf8(process.readAllStandardOutput());
    }
private Q_SLOTS:
    void defaultsPreferTheMatchingSibling()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto sibling = executable(dir.path() + QStringLiteral("/app/dwarfsctl"));
        const auto fromPath = executable(dir.path() + QStringLiteral("/bin/dwarfsctl"));
        QVERIFY(!sibling.isEmpty()); QVERIFY(!fromPath.isEmpty());
        QCOMPARE(resolve(dir.path(), {}, dir.path() + QStringLiteral("/bin")), sibling);
        QVERIFY(QFile::setPermissions(sibling, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QCOMPARE(resolve(dir.path(), {}, dir.path() + QStringLiteral("/bin")), fromPath);
    }
    void explicitOverridesChooseTheirProgram()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QVERIFY(!executable(dir.path() + QStringLiteral("/app/dwarfsctl")).isEmpty());
        const auto named = executable(dir.path() + QStringLiteral("/bin/custom-backend"));
        const auto absolute = executable(dir.path() + QStringLiteral("/selected/backend with spaces"));
        QVERIFY(!named.isEmpty()); QVERIFY(!absolute.isEmpty());
        QCOMPARE(resolve(dir.path(), QStringLiteral("custom-backend"), dir.path() + QStringLiteral("/bin")), named);
        QCOMPARE(resolve(dir.path(), QStringLiteral("  custom-backend  "), dir.path() + QStringLiteral("/bin")), named);
        QCOMPARE(resolve(dir.path(), absolute, dir.path() + QStringLiteral("/bin")), absolute);
        QCOMPARE(resolve(dir.path(), QStringLiteral("  ") + absolute + QStringLiteral("  "), dir.path() + QStringLiteral("/bin")), absolute);
        const auto missing = dir.path() + QStringLiteral("/explicit-missing");
        QCOMPARE(resolve(dir.path(), missing, {}), missing);
    }
    void homeFallbackRequiresAnExecutable()
    {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const auto home = executable(dir.path() + QStringLiteral("/home/.local/bin/dwarfsctl"));
        QVERIFY(!home.isEmpty());
        QCOMPARE(resolve(dir.path(), {}, dir.path() + QStringLiteral("/empty-path")), home);
        const QString unique = QStringLiteral("dwarfsctl-discovery-test-unavailable-82e195");
        QVERIFY(!executable(dir.path() + QStringLiteral("/home/.local/bin/") + unique, false).isEmpty());
        QVERIFY(!executable(dir.path() + QStringLiteral("/bin/") + unique, false).isEmpty());
        QCOMPARE(resolve(dir.path(), unique, dir.path() + QStringLiteral("/bin")), unique);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--resolve"))) {
        const auto result = dwarfsctl_kde::dwarfsctlExe().toUtf8();
        std::fwrite(result.constData(), 1, result.size(), stdout);
        return 0;
    }
    BackendDiscoveryTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_backenddiscovery.moc"
