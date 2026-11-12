// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include "dwarfsctljson.h"

namespace dwarfsplugin {

/// The first non-empty, trimmed line of `s`, or an empty QString.
///
/// Single owner for the "turn a tool payload into one summary line" rule used
/// by the checksum and integrity dialogs. The split is on '\n' only: both
/// dwarfsctl and dwarfsck emit LF on every platform this ships on, and a stray
/// '\r' is removed by the trim.
QString firstNonEmptyLine(const QString &s);

/// The first meaningful line of a tool's output from an envelope.
///
/// Preferred schema: `details.tool.{stderr|stdout}`. Falls back to the raw io
/// that DwarfsctlJob attaches under `details._stderr`/`details._stdout` when a
/// command has no tool payload. stderr wins over stdout because it carries the
/// failure text when a command fails.
QString firstLineFromToolPayload(const Envelope &env);

} // namespace dwarfsplugin
