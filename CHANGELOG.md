# Changelog

This file records release changes.

## 0.13.0 — Unreleased

- Added DwarFS Utility, a standalone Disk Utility-style workspace for inspecting,
  mounting, browsing, extracting, verifying and creating images using the same
  components as Dolphin and the tray, with an exact compression overview and a
  bounded Content Rings visualization of logical image contents.
- Added a toolbar About button to DwarFS Utility, aligned tray menu styling,
  corrected Breeze toolbar icons and fixed Contents clipping in short windows.
- Added application screenshots and a standalone-app introduction to the README.
- Added an Arch Linux package build and clean-install test, with checksummed
  parallel-hashmap headers and preserved source and license material.
- Updated the Fedora package target to Fedora 45 and added native ARM64 package,
  installed-package and upstream reference-image CI coverage.
- Added a direct KIO copy-out path so Dolphin can publish files from a `dwarfs:`
  location without duplicating every byte through a temporary KIO transfer.

## 0.12.20 — 2026-10-16

Initial release.

### Desktop integration

- Added native Dolphin actions for creating, mounting, opening, extracting and
  checking DwarFS images.
- Added a DwarFS page to Dolphin's standard Properties dialog.
- Added read-only `dwarfs:` browsing, MIME identification, thumbnails and a
  compressed-disk file icon.
- Added a Plasma tray application that owns mount state and keeps mounted images
  visible in Dolphin Places.
- Added creation progress, completion summaries and visible mount progress.

### Creation and settings

- Added source-aware compression recommendations with four practical outcomes.
- Added one Advanced User preference for custom profiles and diagnostics.
- Added a profile editor covering the bundled DwarFS writer's supported options.
- Consolidated application defaults in the tray Settings window.
- Stopped creating adjacent `.meta.json` sidecars; older sidecars remain readable
  for their icon hint.

### Engine and packaging

- Replaced the original Rust backend with a C++/Qt backend and a private engine
  built from pinned DwarFS 0.15.7 source.
- Added Fedora 44, Debian 13 and Ubuntu 26.04 packages for x86-64.
- Included corresponding application source, pinned DwarFS source and license
  notices with release artifacts.
- Added package, real-image, D-Bus and KDE component tests on all supported
  distributions.

### Reliability and safety

- Added per-image locking and staged publication for created images, extracted
  files and checksum exports.
- Prevented overwrite operations from following output symlinks.
- Preserved existing destinations unless replacement is explicitly requested.
- Made extraction create and open a clearly named folder, refuse existing
  destinations, and publish only after successful completion.
- Kept mount paths and registry state inside the user's private runtime directory.

### Known limitations

- Images and mounted volumes are read-only.
- `dwarfs:` listings do not expose modification times.
- Mount control requires a graphical session with a D-Bus session bus and FUSE.
- Encryption, signing images and recovery parity are outside the project scope.
