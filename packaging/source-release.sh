#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
out=${1:?usage: source-release.sh OUTPUT_DIR}
mkdir -p "$out"
out=$(realpath "$out")
version=$(cat VERSION)
epoch=${SOURCE_DATE_EPOCH:-}
if [[ -z "$epoch" ]]; then
  if [[ "$(git rev-parse --show-toplevel 2>/dev/null || true)" == "$PWD" ]]; then
    epoch=$(git show -s --format=%ct HEAD)
  else
    # Release tarballs retain the canonical timestamp on VERSION.
    epoch=$(stat -c %Y VERSION)
  fi
fi
[[ "$epoch" =~ ^[0-9]+$ ]] || { echo 'Invalid SOURCE_DATE_EPOCH' >&2; exit 2; }
tar -czf "$out/dwarfs-dolphin-$version-source.tar.gz" \
  --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
  --transform "s,^,dwarfs-dolphin-$version/," \
  VERSION LICENSE LICENSES README.md CONTRIBUTING.md SECURITY.md CHANGELOG.md \
  RELEASE_SIGNING_KEY.asc \
  THIRD_PARTY_NOTICES.md .github .gitignore .editorconfig cpp resources kde \
  tests docs man packaging justfile
