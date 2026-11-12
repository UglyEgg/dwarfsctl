// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

class DwarfsctlRunner {
public:
    struct Result {
        int exit_code = -1;
        bool crashed = false;

        QByteArray stdout_bytes;
        QByteArray stderr_bytes;
    };

    // Back-compat alias for callers that expect DwarfsctlRunner::RunResult.
    using RunResult = Result;

    // Runs dwarfsctl with the given argv (not including the program name).
    // Uses argv-only execution; no shell interpolation.
    static Result run(const QStringList& args, int timeout_ms = 300000);

    // Resolved `[ui]` values for GUI callers, read from the backend
    // (`dwarfsctl config show --section ui`) rather than by parsing the config
    // file here: precedence, environment expansion and defaults are dwarfsctl's
    // and must not have a second implementation.
    //
    // Values of 0 in the config mean "no timeout" and are returned as -1 (Qt
    // convention). When the backend cannot be reached, the documented defaults
    // apply and the GUI keeps working.
    static int defaultTimeoutMs();
    static int buildTimeoutMs();

    // Resolves the dwarfsctl program name/path.
    // If DWARFSCTL_BIN is set in the environment, uses that value; otherwise "dwarfsctl".
    static QString dwarfsctlProgram();

    // Whether mounting an image should open its mountpoint, from the resolved
    // `[ui].open_after_mount` (read through the backend, like the timeouts).
    //
    // Mounting is how a user looks inside an image, so the default is true: the
    // mountpoint opens in the file manager and the image behaves like a disk
    // image rather than a file the user has to go find afterwards. Set the key to
    // false to keep mounting silent.
    static bool openAfterMount();
};
