#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

# KDE translation extraction entrypoint.
# This is intended to be used by KDE/ECM translation tooling.

podir="$(pwd)"

cpp_files=$(find .. -type f \( -name '*.cpp' -o -name '*.h' \) -print)

# C++ sources (KF6 i18n macros).
# shellcheck disable=SC2086
$XGETTEXT --from-code=UTF-8 -kI18N_NOOP -kI18N_NOOP2 -kI18N_NOOP2_NOSTRIP \
  -ki18n -ki18nc:1c,2 -ki18np -ki18ncp:1c,2 \
  -o "${podir}/dwarfsctl-dolphin-plugin.pot" \
  ${cpp_files}

# Desktop/service files (best-effort). Some toolchains use extractrc for these.
# If extractrc is available, include them.
if command -v extractrc >/dev/null 2>&1; then
  desk_files=$(find .. -type f -name '*.desktop' -print)
  if [[ -n "${desk_files}" ]]; then
    # shellcheck disable=SC2086
    extractrc ${desk_files} >> "${podir}/rc.cpp" || true
    $XGETTEXT --from-code=UTF-8 -kI18N_NOOP -kI18N_NOOP2 -kI18N_NOOP2_NOSTRIP \
      -ki18n -ki18nc:1c,2 -ki18np -ki18ncp:1c,2 \
      -o "${podir}/desktop.pot" "${podir}/rc.cpp" || true
    rm -f "${podir}/rc.cpp"
  fi
fi
