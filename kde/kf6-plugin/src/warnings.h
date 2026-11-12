// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include "dwarfsctljson.h"

namespace dwarfsplugin {

// Extract the mkdwarfs version mismatch warning at most once per Dolphin session.
//
// Returns true and writes a message into outMessage when the warning is present
// and has not yet been consumed this session. Returns false otherwise.
//
// Detection is robust and uses the structured warning object code emitted by
// dwarfsctl (not substring matching).
bool takeMkdwarfsWarningOnce(const Envelope &env, QString *outMessage);

} // namespace dwarfsplugin
