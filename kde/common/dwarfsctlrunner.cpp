// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "dwarfsctlrunner.h"
#include "dwarfsctl_exec.h"

QString DwarfsctlRunner::dwarfsctlProgram()
{
    return dwarfsctl_kde::dwarfsctlExe();
}
