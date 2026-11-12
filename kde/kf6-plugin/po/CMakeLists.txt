# SPDX-FileCopyrightText: 2026 Richard Majewski
# SPDX-License-Identifier: GPL-3.0-or-later

# Translation install hook.
# This directory may contain *.po files in downstream packaging.

if (COMMAND ki18n_install)
  ki18n_install(po)
endif()
