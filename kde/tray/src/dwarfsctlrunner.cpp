// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dwarfsctlrunner.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

#include "uiconfig.h"

QString DwarfsctlRunner::dwarfsctlProgram() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString override = env.value(QStringLiteral("DWARFSCTL_BIN"));
    const QString prog = override.trimmed().isEmpty() ? QStringLiteral("dwarfsctl") : override.trimmed();

    // Dolphin launched from a desktop/session environment may not inherit a shell-initialized PATH.
    // Resolve the executable defensively so the plugin doesn't silently become a NOOP.
    if (QDir::isAbsolutePath(prog)) {
        return prog;
    }

    const QString found = QStandardPaths::findExecutable(prog);
    if (!found.isEmpty()) {
        return found;
    }

    const QString homeBin = QDir::home().filePath(QStringLiteral(".local/bin/") + prog);
    if (QFileInfo::exists(homeBin)) {
        return homeBin;
    }

    const QString usrLocalBin = QStringLiteral("/usr/local/bin/") + prog;
    if (QFileInfo::exists(usrLocalBin)) {
        return usrLocalBin;
    }

    // Fall back to the raw program name; the caller will surface a helpful error.
    return prog;
}

int DwarfsctlRunner::defaultTimeoutMs()
{
    // The backend resolves `[ui].command_timeout_ms` (precedence, env
    // expansion, defaults). The mirrored default covers an unreachable backend.
    return dwarfsctl_kde::loadUiConfig(dwarfsctlProgram()).command_timeout_ms;
}

int DwarfsctlRunner::buildTimeoutMs()
{
    return dwarfsctl_kde::loadUiConfig(dwarfsctlProgram()).build_timeout_ms;
}

bool DwarfsctlRunner::openAfterMount()
{
    return dwarfsctl_kde::loadUiConfig(dwarfsctlProgram()).open_after_mount;
}

DwarfsctlRunner::Result DwarfsctlRunner::run(const QStringList& args, int timeout_ms) {
    Result r;

    QProcess p;
    p.setProgram(dwarfsctlProgram());
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);

    p.start();
    if (!p.waitForStarted()) {
        r.exit_code = -1;
        r.crashed = true;
        r.stderr_bytes = QByteArrayLiteral("failed to start dwarfsctl process");
        return r;
    }

    if (!p.waitForFinished(timeout_ms)) {
        p.kill();
        p.waitForFinished(5000);
        r.exit_code = -1;
        r.crashed = true;
        r.stderr_bytes = QByteArrayLiteral("dwarfsctl timed out");
        return r;
    }

    r.exit_code = p.exitCode();
    r.crashed = (p.exitStatus() != QProcess::NormalExit);
    r.stdout_bytes = p.readAllStandardOutput();
    r.stderr_bytes = p.readAllStandardError();
    return r;
}
