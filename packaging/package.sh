#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Package an already tested staging tree on its matching distribution.
set -euo pipefail
stage=$(realpath "${1:?usage: package.sh STAGING_DIR OUTPUT_DIR}")
mkdir -p "${2:?output directory required}"
out=$(realpath "$2")
. /etc/os-release
version=$(cat "$(dirname "$0")/../VERSION")
if [[ "$ID" == fedora ]]; then
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  mkdir -p "$work"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
  cat > "$work/SPECS/package.spec" <<SPEC
Name: dwarfs-dolphin
Version: $version
Release: 1%{?dist}
Summary: Native DwarFS CLI and KDE integration
License: GPL-3.0-or-later AND MIT AND BSL-1.0 AND Apache-2.0
URL: https://github.com/UglyEgg/dwarfsctl
Provides: bundled(dwarfs) = 0.15.7
Provides: dwarfsctl = $version
Requires: fuse3, coreutils, shared-mime-info, desktop-file-utils, qt6-qtwayland, dbus-daemon
Recommends: dolphin
%description
C++ backend, pinned DwarFS engine, Dolphin actions, KIO browsing, tray and thumbnails.
%prep
%build
%install
mkdir -p %{buildroot}
cp -a $stage/. %{buildroot}/
chown -R root:root %{buildroot}
%files
/usr/bin/*
/usr/libexec/dwarfsctl-engine
/usr/lib*/qt6/plugins/*
/usr/lib/systemd/user/*
/usr/share/*
%config(noreplace) /etc/dwarfs-dolphin/config.toml
/etc/xdg/autostart/dwarfsctl-tray.desktop
%changelog
* Sun Nov 15 2026 UglyEgg <uglyegg@users.noreply.github.com> - $version-1
- Improve image creation cancellation and unify the KDE interface.
* Sat Nov 14 2026 UglyEgg <uglyegg@users.noreply.github.com> - 0.13.0-1
- First public release.
SPEC
  rpmbuild -bb --define "_topdir $work" --define '_build_id_links none' --define 'debug_package %{nil}' "$work/SPECS/package.spec"
  find "$work/RPMS" -name '*.rpm' -exec cp '{}' "$out/" \;
else
  [[ "$ID" == debian || "$ID" == ubuntu ]] || { echo 'Supported: Fedora, Debian, Ubuntu' >&2; exit 2; }
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  cp -a "$stage" "$work/payload"
  mkdir -p "$work/debian" "$work/payload/DEBIAN"
  cat > "$work/debian/control" <<CTRL
Source: dwarfs-dolphin
Section: utils
Priority: optional
Maintainer: UglyEgg <uglyegg@users.noreply.github.com>
Homepage: https://github.com/UglyEgg/dwarfsctl
Vcs-Git: https://github.com/UglyEgg/dwarfsctl.git

Package: dwarfs-dolphin
Architecture: any
Description: Native DwarFS CLI and KDE integration
CTRL
  binaries=()
  while IFS= read -r -d '' file; do
    if [[ "$(head -c4 "$file")" == $'\177ELF' ]]; then binaries+=("-e$file"); fi
  done < <(find "$work/payload/usr" -type f -print0)
  deps=$(cd "$work" && dpkg-shlibdeps -O "${binaries[@]}" | sed -n 's/^shlibs:Depends=//p')
  [[ -n "$deps" ]] || { echo 'No shared-library dependencies found' >&2; exit 1; }
  arch=$(dpkg --print-architecture)
  cat > "$work/payload/DEBIAN/control" <<CTRL
Package: dwarfs-dolphin
Version: $version-1+$ID$VERSION_ID
Section: utils
Priority: optional
Architecture: $arch
Maintainer: UglyEgg <uglyegg@users.noreply.github.com>
Homepage: https://github.com/UglyEgg/dwarfsctl
Depends: $deps, fuse3, coreutils, shared-mime-info, desktop-file-utils, qt6-qpa-plugins, qt6-wayland, dbus-daemon
Provides: dwarfsctl (= $version)
Breaks: dwarfsctl (<< 0.12.0), dwarfs-dolphin-integration (<< 0.12.0)
Replaces: dwarfsctl (<< 0.12.0), dwarfs-dolphin-integration (<< 0.12.0)
Recommends: dolphin
Description: Native DwarFS CLI and KDE integration
 C++ backend, pinned DwarFS engine, Dolphin actions, KIO browsing, tray and thumbnails.
CTRL
  echo /etc/dwarfs-dolphin/config.toml > "$work/payload/DEBIAN/conffiles"
  dpkg-deb --root-owner-group --build "$work/payload" "$out/dwarfs-dolphin_${version}-1_${ID}${VERSION_ID}_${arch}.deb"
fi

bash "$(dirname "$0")/source-release.sh" "$out"
cp "$stage/usr/share/dwarfsctl/sources/dwarfs-0.15.7.tar.xz" "$out/"
