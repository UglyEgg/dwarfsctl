# Contributing to DwarFSctl

DwarFSctl combines a C++ backend with KDE Plasma integration. Changes should
keep the desktop interface approachable while preserving the command-line and
D-Bus contracts used by each component.

## Supported build

Podman provides the supported, reproducible build environment:

```sh
./packaging/container-build.sh fedora   # Fedora 45; native x86-64 or ARM64
./packaging/container-build.sh debian   # Debian 13
./packaging/container-build.sh ubuntu   # Ubuntu 26.04
./packaging/container-build.sh arch     # Arch Linux, x86-64
```

Each command builds the complete application, runs its tests and writes a native
package under `build-out/<distribution>/`. It does not install anything on the
host. Run `./packaging/test-package.sh <distribution>` to install that package
inside a disposable matching container and exercise the installed payload.
Fedora defaults to the host architecture; CI qualifies both x86-64 and ARM64.

For an already provisioned Fedora, Debian or Ubuntu development environment:

```sh
./packaging/build-all.sh build/native stage/native
./packaging/package.sh stage/native build-out/native
```

Use the container path for Arch; it also supplies and stages the pinned header
dependency that is absent from Arch's official repositories.

`just guard` runs the fast architecture checks. `just docs` validates repository
links and renders the manual. The full distribution build remains the release
gate because it exercises the bundled DwarFS engine and KDE components together.

## Architecture

- `cpp/cli/` implements the public CLI, configuration, locking and JSON output.
- `cpp/reader/` builds the private engine from checksum-pinned DwarFS source.
- `kde/kf6-plugin/` provides Dolphin actions and the Properties extension.
- `kde/tray/` owns mount state, settings and the session D-Bus service.
- `kde/manager/` provides the additive standalone image-management workspace;
  it reuses the Dolphin and tray components and does not own mount state.
- `kde/kio-worker/` exposes read-only `dwarfs:` locations.
- `kde/thumbnailer/` and `kde/mime/` provide file previews and identification.
- `packaging/` contains container builds and native package assembly.

All image operations pass through `dwarfsctl`. KDE components do not parse the
configuration file, reproduce DwarFS option tables or invoke a shell. Mount and
unmount requests use the tray's Mounts1 service; the tray is the only process
that changes mount state.

The public CLI always writes one JSON envelope to standard output. Diagnostics
and build progress use standard error. Avoid changing command names, exit codes,
envelope fields or the Mounts1 interface without a compatibility plan and
matching contract tests.

## Tests

The main checks are:

- `tests/cli/contracts.py` for the installed CLI surface
- `tests/cli/reader_contracts.py` for real images and extraction
- component CTest suites under `kde/`
- `tests/dbus_contract_check.sh` for the Mounts1 boundary
- `tests/guards.py` for cross-component architecture rules
- `tests/docs_check.py` for public repository links and documentation boundaries

Interactive FUSE and Dolphin behavior must also be checked in a real Plasma
session before a public release. Container tests cannot establish that Plasma
has discovered plugins, MIME data, icons or the session mount service.

Release manifests are signed with the GPG key published as
`RELEASE_SIGNING_KEY.asc`. The full fingerprint is
`60C38F37374C2B51BE7CA8CC698CE0575AED79AA`. SSH keys used to access Git remotes
are not release-signing keys.

## Source and licensing

DwarFS 0.15.7 is downloaded from its upstream release using the checksum in the
build scripts. Packages ship the exact source archive and applicable licenses.
Changes to the pin require a source, license, image-compatibility and option-schema
review.

Keep SPDX headers and third-party notices intact. New dependencies must be
compatible with GPL-3.0-or-later redistribution on every supported distribution.

## Submitting changes

Keep commits focused and describe the behavior a reviewer should observe. Include
the narrow test that exercises the change and run the appropriate complete
distribution build before proposing a release. User-facing text should describe
tasks and outcomes; implementation history belongs in commits and pull requests,
not product documentation.
