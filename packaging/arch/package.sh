#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../.." && pwd)
export DWARFSCTL_STAGE=$(realpath "${1:?staging directory required}")
mkdir -p "${2:?output directory required}"
out=$(realpath "$2")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$repo/packaging/arch/PKGBUILD" "$work/"
version=$(<"$repo/VERSION")
sed -i "s/^pkgver=.*/pkgver=$version/" "$work/PKGBUILD"
if [[ $EUID == 0 ]]; then
    chown nobody:nobody "$work"
    (cd "$work"; runuser -u nobody -- env DWARFSCTL_STAGE="$DWARFSCTL_STAGE" PACKAGER='Richard Majewski <uglyegg@users.noreply.github.com>' makepkg --nodeps --noconfirm)
else
    (cd "$work"; PACKAGER='Richard Majewski <uglyegg@users.noreply.github.com>' makepkg --nodeps --noconfirm)
fi
find "$work" -maxdepth 1 -name '*.pkg.tar.zst' -exec cp '{}' "$out/" \;
cp "$DWARFSCTL_STAGE/usr/share/dwarfsctl/sources/dwarfs-0.15.10.tar.xz" "$out/"
cp "$DWARFSCTL_STAGE/usr/share/dwarfsctl/sources/parallel-hashmap-2.0.0.tar.gz" "$out/"
