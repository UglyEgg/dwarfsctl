#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
build=${1:?usage: build-all.sh BUILD_DIR STAGING_DIR}
stage=${2:?staging directory required}
mkdir -p "$build"
build=$(cd "$build" && pwd)
stage=$(realpath -m "$stage")
if [[ "$stage" == / || "$stage" == "$repo" ]]; then
  echo "refusing unsafe staging directory: $stage" >&2
  exit 2
fi
# The package payload must reflect the current install manifests exactly.
# Reusing a populated tree retains files removed since the previous build.
rm -rf -- "$stage"
mkdir -p "$stage"
bash packaging/build-reader.sh "$build/native" "$stage"
test_home=$(mktemp -d)
trap 'rm -rf "$test_home"' EXIT
export HOME="$test_home" XDG_CONFIG_HOME="$test_home/config" XDG_CACHE_HOME="$test_home/cache" XDG_DATA_HOME="$test_home/data" XDG_RUNTIME_DIR="$test_home/run"
mkdir -m700 "$XDG_RUNTIME_DIR"
export DWARFSCTL_ENGINE="$stage/usr/libexec/dwarfsctl-engine"
export DWARFSCTL_BIN="$stage/usr/bin/dwarfsctl"
export DWARFSCTL_INTEGRATION=1 QT_QPA_PLATFORM=offscreen
export PATH="$stage/usr/bin:$PATH"
python3 tests/cli/contracts.py "$DWARFSCTL_BIN" "$DWARFSCTL_ENGINE"
for part in tray kf6-plugin manager thumbnailer kio-worker open-image; do
  cmake -S "kde/$part" -B "$build/$part" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=ON -DBUILD_TESTING=ON
  cmake --build "$build/$part" --parallel "${JOBS:-2}"
  ctest --test-dir "$build/$part" --output-on-failure
  DESTDIR="$stage" cmake --install "$build/$part"
done
install -Dm644 man/dwarfsctl.1 "$stage/usr/share/man/man1/dwarfsctl.1"
install -Dm644 kde/mime/packages/application-x-dwarfs.xml "$stage/usr/share/mime/packages/application-x-dwarfs.xml"
install -Dm644 packaging/etc/dwarfs-dolphin/config.toml "$stage/etc/dwarfs-dolphin/config.toml"
install -Dm644 kde/tray/dwarfsctl-tray.desktop "$stage/usr/share/applications/dwarfsctl-tray.desktop"
install -Dm644 kde/tray/dwarfsctl-tray.desktop "$stage/etc/xdg/autostart/dwarfsctl-tray.desktop"
mkdir -p "$stage/usr/share/dbus-1/services"
sed 's|@EXEC@|/usr/bin/dwarfsctl-tray|g' kde/tray/org.quest.entropy.dwarfsctl.Tray.service.in > "$stage/usr/share/dbus-1/services/org.quest.entropy.dwarfsctl.Tray.service"
install -Dm644 LICENSE "$stage/usr/share/licenses/dwarfsctl/LICENSE"
install -Dm644 THIRD_PARTY_NOTICES.md "$stage/usr/share/licenses/dwarfsctl/THIRD_PARTY_NOTICES.md"

install -Dm644 kde/mime/icons/dwarfsctlimage.svg "$stage/usr/share/icons/hicolor/scalable/mimetypes/dwarfsctlimage.svg"
for size in 16 22 24 32 48 64 128; do
  install -Dm644 kde/mime/icons/dwarfsctlimage.svg \
    "$stage/usr/share/icons/hicolor/${size}x${size}/mimetypes/dwarfsctlimage.svg"
done
