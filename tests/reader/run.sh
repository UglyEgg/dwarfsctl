#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cli=${1:?usage: run.sh CLI READER CACHE_DIR}
reader=${2:?}
cache=${3:?}
mkdir -p "$cache"
case "$(uname -m)" in
  x86_64) upstream_arch=x86_64; checksum=4dc3b756af88ede837eb3f27fbb5f2f0dbb83d7c9933a1d3be9216d2069d2f5f ;;
  aarch64) upstream_arch=aarch64; checksum=5957c2a380e1c57bc3b18475612be36120c3faf786373fcdf7c15245654869db ;;
  *) echo "Unsupported reference-tool architecture: $(uname -m)" >&2; exit 2 ;;
esac
archive="$cache/dwarfs-tools-0.15.7.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl -fL --retry 3 -o "$archive.part" "https://github.com/mhx/dwarfs/releases/download/v0.15.7/dwarfs-0.15.7-Linux-$upstream_arch.tar.xz"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c -
tar -xJf "$archive" -C "$cache"
tools="$cache/dwarfs-0.15.7-Linux-$upstream_arch/bin"
python3 "$(dirname "$0")/../cli/reader_contracts.py" "$cli" "$reader" "$tools/mkdwarfs" "$tools/dwarfsck"
