#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

cd -- "$(dirname -- "$0")/.."
destination=${1:?usage: release-artifacts.sh DESTINATION BUILD_OUTPUT...}
shift
(( $# > 0 )) || { echo "at least one build output directory is required" >&2; exit 2; }

version=$(<VERSION)
signing_key=${DWARFSCTL_SIGNING_KEY:-60C38F37374C2B51BE7CA8CC698CE0575AED79AA}
mkdir -p "$destination"
destination=$(realpath "$destination")

copy_verified() {
  local source=$1 target
  target="$destination/$(basename "$source")"
  if [[ -e "$target" ]]; then
    cmp --silent "$source" "$target" || {
      echo "conflicting release artifact: $(basename "$source")" >&2
      exit 1
    }
  else
    cp -- "$source" "$target"
  fi
}

found_rpm_x86=0
found_rpm_arm=0
found_debian=0
found_ubuntu=0
found_arch=0
found_phmap=0
found_source=0
found_upstream=0
for output in "$@"; do
  [[ -d "$output" ]] || { echo "not a directory: $output" >&2; exit 2; }
  while IFS= read -r -d '' artifact; do
    name=$(basename "$artifact")
    case "$name" in
      dwarfs-dolphin-"$version"-*.fc45.x86_64.rpm) found_rpm_x86=1 ;;
      dwarfs-dolphin-"$version"-*.fc45.aarch64.rpm) found_rpm_arm=1 ;;
      dwarfs-dolphin_"$version"-*_debian*_amd64.deb) found_debian=1 ;;
      dwarfs-dolphin_"$version"-*_ubuntu*_amd64.deb) found_ubuntu=1 ;;
      dwarfs-dolphin-"$version"-source.tar.gz) found_source=1 ;;
      dwarfs-0.15.10.tar.xz) found_upstream=1 ;;
      dwarfs-dolphin-"$version"-*-x86_64.pkg.tar.zst) found_arch=1 ;;
      parallel-hashmap-2.0.0.tar.gz) found_phmap=1 ;;
      *) continue ;;
    esac
    copy_verified "$artifact"
  done < <(find "$output" -maxdepth 1 -type f -print0)
done

(( found_rpm_x86 && found_rpm_arm && found_debian && found_ubuntu && found_arch && found_phmap && found_source && found_upstream )) || {
  echo "release set is incomplete" >&2
  exit 1
}

(
  cd "$destination"
  find . -maxdepth 1 -type f \
    ! -name SHA256SUMS ! -name 'SHA256SUMS.asc' \
    -printf '%f\n' | LC_ALL=C sort | xargs -r sha256sum > SHA256SUMS

  gpg --list-secret-keys "$signing_key" >/dev/null 2>&1 || {
    echo "GPG release-signing key is unavailable: $signing_key" >&2
    exit 1
  }
  gpg --yes --armor --detach-sign --local-user "$signing_key" \
    --output SHA256SUMS.asc SHA256SUMS
  gpg --verify SHA256SUMS.asc SHA256SUMS
)

printf 'Release artifacts assembled in %s\n' "$destination"
