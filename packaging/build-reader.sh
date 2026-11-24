#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?usage: build-reader.sh BUILD_DIR [STAGING_DIR]}
mkdir -p "$build"
build=$(cd -- "$build" && pwd)
version=0.15.10
sha=036109bc210b5afa593a005f798918aacdd412a3d9dbf703bed2e70477454027
archive="$build/dwarfs-$version.tar.xz"
if [[ ! -f "$archive" ]]; then
    curl --fail --location --retry 3 -o "$archive.part" \
        "https://github.com/mhx/dwarfs/releases/download/v$version/dwarfs-$version.tar.xz"
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check -
# Require every supported decoder; upstream otherwise silently omits optional ones.
pkg-config --exists libcrypto libarchive libxxhash libzstd liblz4 liblzma libbrotlidec libbrotlienc flac++
if [[ ! -d "$build/dwarfs-$version" ]]; then tar -xJf "$archive" -C "$build"; fi
# Apply our pinned compatibility change strictly; accept an already patched tree.
compat_patch="$repo/packaging/patches/dwarfs-$version-selected-directories.patch"
if patch --dry-run --silent --fuzz=0 --forward -p1 -d "$build/dwarfs-$version" < "$compat_patch"; then
    patch --batch --fuzz=0 --forward -p1 -d "$build/dwarfs-$version" < "$compat_patch"
elif ! patch --dry-run --silent --fuzz=0 --reverse -p1 -d "$build/dwarfs-$version" < "$compat_patch"; then
    echo "DwarFS compatibility patch does not match the source tree" >&2
    exit 1
fi
cmake -S "$build/dwarfs-$version" -B "$build/cmake" -G Ninja \
    -DZSTD_STATIC_LIB=OFF -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX=/usr -DDWARFSCTL_READER_SOURCE="$repo/cpp/reader" \
    -DCMAKE_PROJECT_dwarfs_INCLUDE="$repo/cpp/reader/project-hook.cmake" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DWITH_LIBDWARFS=ON -DWITH_TOOLS=ON -DWITH_FUSE_DRIVER=ON \
    -DWITH_TESTS=OFF -DWITH_BENCHMARKS=OFF -DWITH_MAN_PAGES=OFF \
    -DWITH_MAN_OPTION=OFF -DWITH_DESKTOP_INTEGRATION=OFF \
    -DUSE_JEMALLOC=OFF -DENABLE_PERFMON=OFF -DDISABLE_CCACHE=ON -DDISABLE_MOLD=ON
cmake --build "$build/cmake" --target dwarfsctl-reader dwarfsctl --parallel "${JOBS:-2}"
if [[ $# -ge 2 ]]; then
    mkdir -p "$2"
    stage=$(cd -- "$2" && pwd)
    DESTDIR="$stage" cmake --install "$build/cmake" --component dwarfsctl-reader
    python3 "$repo/packaging/reader-notices.py" "$stage"
    # Ship the pinned source archive, compatibility patch, and all notices.
    install -Dm644 "$archive" "$stage/usr/share/dwarfsctl/sources/dwarfs-$version.tar.xz"
    install -Dm644 "$compat_patch" "$stage/usr/share/dwarfsctl/sources/$(basename "$compat_patch")"
    install -Dm644 "$repo/packaging/patches/dwarfs-$version-extraction-names.patch" \
        "$stage/usr/share/dwarfsctl/sources/dwarfs-$version-extraction-names.patch"
fi
