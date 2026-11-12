#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
. packaging/targets.sh
. /etc/os-release
distro=$ID
build_only=false
for arg in "$@"; do
 case "$arg" in
  --build-only) build_only=true ;;
  --test|--system|--with-kf6-plugin|--with-thumbnailer|--with-kio-worker) : ;;
  --help|-h) echo 'Usage: packaging/install.sh [--build-only] [--system] [--test]'; echo 'Builds and tests the complete C++ application for this distribution, then installs its package with PolicyKit. --build-only stops before installation.'; exit 0 ;;
  *) echo "Unsupported option: $arg (see packaging/README.md)" >&2; exit 2 ;;
 esac
done
case "$ID:$VERSION_ID" in
 fedora:$FEDORA_RELEASE|debian:13|ubuntu:26.04) : ;;
 *) echo "No tested package target for $ID $VERSION_ID" >&2; exit 2 ;;
esac
bash packaging/container-build.sh "$distro"
$build_only && exit 0
version=$(<VERSION)
case "$distro" in
 fedora)
  host_arch=$(rpm --eval '%{_arch}')
  [[ "$host_arch" == "$FEDORA_ARCH" ]] || { echo "Fedora target architecture $FEDORA_ARCH does not match this host ($host_arch)" >&2; exit 2; }
  package="$PWD/build-out/fedora/dwarfs-dolphin-$version-1.fc$FEDORA_RELEASE.$host_arch.rpm"
  ;;
 debian) package="$PWD/build-out/debian/dwarfs-dolphin_${version}-1_debian13_amd64.deb" ;;
 ubuntu) package="$PWD/build-out/ubuntu/dwarfs-dolphin_${version}-1_ubuntu26.04_amd64.deb" ;;
esac
[[ -f "$package" ]] || { echo "Built package not found: $package" >&2; exit 1; }
case "$distro" in
 fedora) pkexec /usr/bin/dnf install "$package" ;;
 debian|ubuntu) pkexec /usr/bin/apt-get install "$package" ;;
esac
