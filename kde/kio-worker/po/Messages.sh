#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

# KDE translation extraction entrypoint for the KIO worker.
# This is intended to be used by KDE/ECM translation tooling.

podir="$(pwd)"

# Only this component's sources: the worker is its own deliverable, so its
# catalogue should not absorb strings from the plugin or the tray.
cpp_files=$(find ../src ../tests -type f \( -name '*.cpp' -o -name '*.h' \) -print)

# C++ sources (KF6 i18n macros).
# shellcheck disable=SC2086
$XGETTEXT --from-code=UTF-8 -kI18N_NOOP -kI18N_NOOP2 -kI18N_NOOP2_NOSTRIP \
  -ki18n -ki18nc:1c,2 -ki18np -ki18ncp:1c,2 \
  -o "${podir}/dwarfsctl-kio-worker.pot" \
  ${cpp_files}
