#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared package target policy. Callers may override these values explicitly for
# CI or cross-architecture container builds; public defaults remain single-source.
FEDORA_RELEASE=${FEDORA_RELEASE:-45}
FEDORA_ARCH=${FEDORA_ARCH:-$(uname -m)}

case "$FEDORA_RELEASE" in
  45) ;;
  *) echo "Unsupported Fedora release: $FEDORA_RELEASE" >&2; return 2 2>/dev/null || exit 2 ;;
esac
case "$FEDORA_ARCH" in
  x86_64|aarch64) ;;
  *) echo "Unsupported Fedora architecture: $FEDORA_ARCH" >&2; return 2 2>/dev/null || exit 2 ;;
esac
