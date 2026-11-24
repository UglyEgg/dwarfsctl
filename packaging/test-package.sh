#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install only into a disposable container, never onto the host.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
. packaging/targets.sh
distro=${1:?usage: test-package.sh fedora|debian|ubuntu|arch}
version=$(cat VERSION)
case "$distro" in
 fedora)
  podman run --arch "$FEDORA_ARCH" --rm --security-opt label=disable -v "$PWD:/work:ro" "registry.fedoraproject.org/fedora:$FEDORA_RELEASE" bash -c \
   "dnf --setopt=zchunk=False --setopt=retries=5 --setopt=timeout=60 install -y python3 /work/build-out/fedora/dwarfs-dolphin-$version-1.fc$FEDORA_RELEASE.$FEDORA_ARCH.rpm && bash /work/tests/package-smoke.sh" ;;
 debian|ubuntu)
  if [[ "$distro" == debian ]]; then base=debian:trixie-slim; release=13; else base=ubuntu:26.04; release=26.04; fi
  podman run --rm --security-opt label=disable -v "$PWD:/work:ro" "$base" bash -c \
   "apt-get update && apt-get install -y --no-install-recommends python3 libglib2.0-bin libkf6iconthemes-bin /work/build-out/$distro/dwarfs-dolphin_${version}-1_${distro}${release}_amd64.deb && bash /work/tests/package-smoke.sh" ;;
 arch)
  podman run --rm --security-opt label=disable -v "$PWD:/work:ro" docker.io/library/archlinux:base bash -c \
    "set -e; source /work/packaging/arch/PKGBUILD; pacman -Syu --noconfirm; pacman -S --needed --noconfirm \"\${depends[@]}\" glib2 python; pacman -U --noconfirm /work/build-out/arch/dwarfs-dolphin-$version-1-x86_64.pkg.tar.zst; bash /work/tests/package-smoke.sh" ;;
 *) echo 'Expected fedora, debian, ubuntu or arch' >&2; exit 2 ;;
esac
