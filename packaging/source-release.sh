#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
out=${1:?usage: source-release.sh OUTPUT_DIR}
mkdir -p "$out"
out=$(realpath "$out")
version=$(cat VERSION)
. packaging/source-date-epoch.sh
tar -czf "$out/dwarfs-dolphin-$version-source.tar.gz" \
  --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner \
  --transform "s,^,dwarfs-dolphin-$version/," \
  VERSION LICENSE LICENSES README.md CONTRIBUTING.md SECURITY.md CHANGELOG.md \
  RELEASE_SIGNING_KEY.asc \
  THIRD_PARTY_NOTICES.md .github .gitignore .editorconfig cpp resources kde \
  tests docs man packaging justfile
