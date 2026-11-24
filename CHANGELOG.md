# Changelog

This file records release changes.

## 0.14.0

- Kept mount status consistent across Utility, Dolphin and the command line,
  including recovery when the tray restarts or a status request cannot complete.
- Made cancellation and mount-service checks responsive, preserved completed
  results during late cancellation, and cleaned up interrupted Dolphin copies.
- Improved browsing of large images with targeted metadata queries, a bounded
  listing cache and a lighter file-list model.
- Validated stored filenames on every extraction path before writing files,
  rejected options that change the requested operation, and preserved
  destination-conflict information for Dolphin's copy handling.

- Recovered interrupted mount registration after a tray restart and stopped
  transient mount launchers when the tray exits.
- Fixed unmounting after the source image is deleted, prevented default mount
  name collisions, and supported image filenames at the filesystem name limit.
- Refused to reuse a mounted image after its source file changes, preserving the
  existing volume and explaining when unmounting and mounting again is needed.

- Waited for owned worker cleanup before reporting completion, failure or
  cancellation, and distinguished a runtime crash from a failure to start.
- Required a real output file before offering creation success or Show in Folder.
- Fixed recent-history settings becoming stale after preference-file replacement,
  and preserved malformed preferences instead of silently rewriting them.
- Fixed Dolphin browsing through folders whose names end in `.dwarfs`, and
  invalidated cached listings when the source image changes.

- Shared recent-image history between Utility and the tray, with one recent
  limit and Off setting. Existing lists are merged automatically.
- Kept unavailable images visible with a Locate action, and preserved access to
  mounted volumes when their source image is moved or removed.
- Distinguished open images from recent history, added Show Image in Folder,
  and clarified that removing entries never deletes files or unmounts volumes.

- Unified progress, elapsed time, cancellation and results across image creation,
  extraction, integrity checks and checksums. Failures stay in the operation
  window with optional details.
- Let a measured creation preview become the finished image with one click,
  without recompressing it. Closing the preview discards the temporary image.
- Kept the tray responsive during its mount actions and removed misleading
  cancellation from mount-service requests. Mount timeouts now explain that the
  outcome is unconfirmed.

- Added a Files tab to DwarFS Utility for browsing image folders and extracting
  selected files or folders without mounting the image.
- Renamed Contents to Size Map and preserved the previously selected view.
- Added extraction cancellation and completion feedback, with a direct action
  to open the extracted folder. Extraction keeps the original folder paths and
  publishes into a new folder without overwriting an existing destination.

## 0.13.1 — 2026-11-15

- Aligned Utility and tray styling, improved theme contrast, and kept image
  details readable in smaller windows.
- Fixed image-creation cancellation leaving a worker running in the background.
  Cancellation now stops the owned processes and cleans up staged output before
  the job reports cancellation.
- Added Cancel to the image-creation progress window. User cancellation closes
  normally; genuine failures and timeouts remain visible as errors.

## 0.13.0 — 2026-11-14

- Added a Change Log tab to About, showing the release history offline.
- Added DwarFS Utility, a standalone Disk Utility-style workspace for inspecting,
  mounting, browsing, extracting, verifying and creating images using the same
  components as Dolphin and the tray, with an exact compression overview and a
  bounded Content Rings visualization of logical image contents.
- Added a toolbar About button to DwarFS Utility, aligned tray menu styling,
  corrected Breeze toolbar icons and fixed Contents clipping in short windows.
- Added application screenshots and a standalone-app introduction to the README.
- Added an Arch Linux package build and clean-install test, with checksummed
  parallel-hashmap headers and preserved source and license material.
- Made corresponding-source archives reproducible across distribution builds.
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
