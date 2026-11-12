#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run after installing the package into a disposable matching distro container.
set -euo pipefail
test_home=$(mktemp -d)
# The activated tray can still write shutdown state after its bus closes.
# Removing the disposable container cleans any remaining temporary state.
trap 'rm -rf "$test_home" 2>/dev/null || true' EXIT
export HOME="$test_home" XDG_CONFIG_HOME="$test_home/config" XDG_CACHE_HOME="$test_home/cache" XDG_DATA_HOME="$test_home/data" XDG_RUNTIME_DIR="$test_home/run"
unset DWARFSCTL_ENGINE DWARFSCTL_BIN
mkdir -m700 "$XDG_RUNTIME_DIR"
if command -v rpm >/dev/null 2>&1; then
  /usr/bin/rpm -V dwarfs-dolphin
fi
for executable in /usr/bin/dwarfsctl /usr/libexec/dwarfsctl-engine /usr/bin/dwarfsctl-tray /usr/bin/dwarfsctl-manager /usr/bin/dwarfsctl-thumbnailer /usr/bin/dwarfsctl-open; do
  test -x "$executable"
  if ldd "$executable" | grep 'not found'; then exit 1; fi
done
for tool in dwarfs mkdwarfs dwarfsck dwarfsextract; do
  if command -v "$tool"; then echo 'Unexpected external DwarFS tool' >&2; exit 1; fi
done
/usr/bin/dwarfsctl --version
mkdir "$test_home/src"
printf 'package installation test\n' > "$test_home/src/example.txt"
/usr/bin/dwarfsctl build "$test_home/src" --output "$test_home/test.dwarfs" --mkdwarfs-arg=--compress-level=1
mime_type=$(gio info --attributes=standard::content-type "$test_home/test.dwarfs" | sed -n 's/^[[:space:]]*standard::content-type: //p')
test "$mime_type" = "application/x-dwarfs"
/usr/bin/dwarfsctl fsck "$test_home/test.dwarfs"
/usr/bin/dwarfsctl ls "$test_home/test.dwarfs"
/usr/bin/dwarfsctl usage "$test_home/test.dwarfs"
/usr/bin/dwarfsctl cat "$test_home/test.dwarfs" /example.txt "$test_home/copy.txt"
test "$(sha256sum < "$test_home/src/example.txt")" = "$(sha256sum < "$test_home/copy.txt")"
/usr/bin/dwarfsctl extract "$test_home/test.dwarfs" "$test_home/extracted"
test "$(sha256sum < "$test_home/src/example.txt")" = "$(sha256sum < "$test_home/extracted/example.txt")"
/usr/bin/dwarfsctl checksum export "$test_home/test.dwarfs" --algo sha256 --output "$test_home/checksums"
/usr/bin/dwarfsctl checksum verify "$test_home/test.dwarfs" --algo sha256 --input "$test_home/checksums" --dir "$test_home/extracted"
test -f /usr/share/applications/dwarfsctl-open.desktop
test -f /usr/share/applications/org.quest.entropy.dwarfsctl.Manager.desktop
desktop-file-validate /usr/share/applications/org.quest.entropy.dwarfsctl.Manager.desktop
grep -qx 'Exec=dwarfsctl-manager %F' /usr/share/applications/org.quest.entropy.dwarfsctl.Manager.desktop
QT_QPA_PLATFORM=offscreen /usr/bin/dwarfsctl-manager --version
properties_plugin=$(find /usr/lib /usr/lib64 -type f \
  -path '*/qt6/plugins/kf6/propertiesdialog/dwarfspropertiesplugin.so' \
  -print -quit 2>/dev/null)
test -n "$properties_plugin"
if find /usr/lib /usr/lib64 -type f \
    -path '*/qt6/plugins/kf6/kpropertiesdialog/dwarfspropertiesplugin.so' \
    -print -quit 2>/dev/null | grep -q .; then
  echo 'DwarFS Properties plugin installed in obsolete KF6 namespace' >&2
  exit 1
fi
test -f /usr/share/icons/hicolor/scalable/mimetypes/dwarfsctlimage.svg
for size in 16 22 24 32 48 64 128; do
  test -f "/usr/share/icons/hicolor/${size}x${size}/mimetypes/dwarfsctlimage.svg"
done
grep -q 'application/x-dwarfs' /usr/share/mime/globs2
grep -q 'dwarfsctlimage' /usr/share/mime/icons
mkdir -p "$XDG_DATA_HOME/icons/GenericFirst/32x32/mimetypes" "$XDG_CONFIG_HOME"
cat > "$XDG_DATA_HOME/icons/GenericFirst/index.theme" <<'EOF'
[Icon Theme]
Name=GenericFirst
Inherits=hicolor
Directories=32x32/mimetypes

[32x32/mimetypes]
Size=32
Context=MimeTypes
Type=Fixed
EOF
cat > "$XDG_DATA_HOME/icons/GenericFirst/32x32/mimetypes/application-x-generic.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32">
  <rect width="32" height="32" fill="#fff"/>
</svg>
EOF
printf '[Icons]\nTheme=GenericFirst\n' > "$XDG_CONFIG_HOME/kdeglobals"
command -v kiconfinder6 >/dev/null
resolved_mime_icon=$(QT_QPA_PLATFORM=offscreen kiconfinder6 dwarfsctlimage)
case "$resolved_mime_icon" in
  /usr/share/icons/hicolor/*/mimetypes/dwarfsctlimage.svg) ;;
  *) echo "Unexpected DwarFS MIME icon: $resolved_mime_icon" >&2; exit 1 ;;
esac
test -f /usr/share/icons/hicolor/scalable/apps/dwarfsctl.svg
grep -qx 'Icon=dwarfsctl' /usr/share/applications/dwarfsctl-tray.desktop
test -f /usr/share/dbus-1/services/org.quest.entropy.dwarfsctl.Tray.service
test -f /usr/share/dwarfsctl/sources/dwarfs-0.15.7.tar.xz
QT_QPA_PLATFORM=offscreen /usr/bin/dwarfsctl-thumbnailer "$test_home/test.dwarfs" "$test_home/thumbnail.png" 128
test -s "$test_home/thumbnail.png"
QT_QPA_PLATFORM=offscreen dbus-run-session -- /usr/bin/dwarfsctl mount list
echo 'Installed package smoke test passed'
