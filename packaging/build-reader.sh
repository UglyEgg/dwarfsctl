#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?usage: build-reader.sh BUILD_DIR [STAGING_DIR]}
mkdir -p "$build"
build=$(cd -- "$build" && pwd)
version=0.15.7
sha=363c7fdbf7bad490a6b8d63186da8643c1aeb17ca54cce1193d7b0ebc57bc6bd
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
    # Ship the exact source used for the embedded libraries and all its notices.
    install -Dm644 "$archive" "$stage/usr/share/dwarfsctl/sources/dwarfs-$version.tar.xz"
fi
