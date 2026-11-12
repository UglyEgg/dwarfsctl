#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
. packaging/targets.sh
. packaging/source-date-epoch.sh
distro=${1:-fedora}
case "$distro" in
 fedora) recipe=fedora; base=registry.fedoraproject.org/fedora:$FEDORA_RELEASE ;;
 debian) recipe=debian; base=debian:trixie-slim ;;
 ubuntu) recipe=debian; base=ubuntu:26.04 ;;
 arch) recipe=arch ;;
 *) echo 'Expected fedora, debian, ubuntu or arch' >&2; exit 2 ;;
esac
if [[ "$distro" == arch ]]; then
  podman build -t localhost/dwarfsctl-desktop-arch -f packaging/containers/reader/Containerfile.arch .
  podman run --rm --env "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" --security-opt label=disable -v "$PWD:/work" localhost/dwarfsctl-desktop-arch bash -c \
    './packaging/build-all.sh build/arch stage/arch && install -Dm644 /tmp/phmap.tar.gz stage/arch/usr/share/dwarfsctl/sources/parallel-hashmap-2.0.0.tar.gz && bash packaging/arch/package.sh stage/arch build-out/arch && bash packaging/source-release.sh build-out/arch'
  exit
fi
build_arch=()
if [[ "$distro" == fedora ]]; then build_arch=(--arch "$FEDORA_ARCH"); fi
podman build "${build_arch[@]}" --build-arg "BASE=$base" -t "localhost/dwarfsctl-reader-$distro" -f "packaging/containers/reader/Containerfile.$recipe" .
podman build "${build_arch[@]}" --build-arg "BASE=localhost/dwarfsctl-reader-$distro" -t "localhost/dwarfsctl-desktop-$distro" -f "packaging/containers/reader/Containerfile.desktop-$recipe" .
podman run "${build_arch[@]}" --rm --env "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH" --security-opt label=disable -v "$PWD:/work" "localhost/dwarfsctl-desktop-$distro" bash -c \
 "./packaging/build-all.sh build/$distro stage/$distro && ./packaging/package.sh stage/$distro build-out/$distro"
