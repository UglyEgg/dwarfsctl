#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Source from the project root, on the host before crossing a container boundary.
if [[ -z "${SOURCE_DATE_EPOCH:-}" ]]; then
  if [[ "$(git rev-parse --show-toplevel 2>/dev/null || true)" == "$PWD" ]]; then
    SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)
  else
    SOURCE_DATE_EPOCH=$(stat -c %Y VERSION)
  fi
fi
[[ "$SOURCE_DATE_EPOCH" =~ ^[0-9]+$ ]] || { echo 'Invalid SOURCE_DATE_EPOCH' >&2; return 2; }
export SOURCE_DATE_EPOCH
