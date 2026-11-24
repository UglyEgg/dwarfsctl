// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QString>

class DwarfsctlRunner {
public:
    // Uses the shared sibling/PATH resolver and explicit DWARFSCTL_BIN override.
    static QString dwarfsctlProgram();
};
