// SPDX-FileCopyrightText: 2026 Richard Majewski
// SPDX-License-Identifier: GPL-3.0-or-later

#include "warnings.h"

// No UI here. Caller decides how to surface the warning (banner vs. modal).

#include <KLocalizedString>

namespace dwarfsplugin {

namespace {

static bool s_mkdwarfsWarned = false;

static const QString kMkdwarfsNewerThanSupported = QStringLiteral("mkdwarfs_newer_than_supported");

} // namespace

bool takeMkdwarfsWarningOnce(const Envelope &env, QString *outMessage)
{
    if (s_mkdwarfsWarned) {
        return false;
    }

    for (const auto &w : env.warning_objects) {
        if (w.code == kMkdwarfsNewerThanSupported) {
            s_mkdwarfsWarned = true;

            if (outMessage) {
                *outMessage = w.message.isEmpty()
                                 ? i18n("A newer version of mkdwarfs was detected. Some features may be missing or incompatible.")
                                 : w.message;
            }
            return true;
        }
    }

    return false;
}

} // namespace dwarfsplugin
