# Packaging

Build a native package in its matching container:

```sh
./packaging/container-build.sh fedora   # or debian, ubuntu
```

Artifacts are written to `build-out/<distribution>/`. The build includes the
CLI, private engine, Plasma tray, Dolphin extensions, KIO worker, thumbnailer,
MIME data, icons, configuration, manual, source and license material.

Fedora builds target release 45 and default to the host CPU architecture. The
supported Fedora architectures are x86-64 and ARM64; CI builds and installs
both. `FEDORA_RELEASE` and `FEDORA_ARCH` are explicit automation overrides, not
an assurance that arbitrary releases or architectures are supported.

Run the installed-package smoke test without changing the host:

```sh
./packaging/test-package.sh fedora      # or debian, ubuntu
```

`./packaging/install.sh --build-only` builds for the host distribution.
`./packaging/install.sh --system` builds and tests first, then asks the
distribution package manager to install through PolicyKit.

After all five native packages and their corresponding source archives have been
qualified, assemble and sign the release set with:

```sh
./packaging/release-artifacts.sh ./release \
  build-out/fedora-x86_64 build-out/fedora-aarch64 \
  build-out/debian build-out/ubuntu build-out/arch
```

The script creates `SHA256SUMS` and `SHA256SUMS.asc`. It uses GPG key
`60C38F37374C2B51BE7CA8CC698CE0575AED79AA` by default; set
`DWARFSCTL_SIGNING_KEY` to select another secret key explicitly. It never uses
the SSH keys configured for Git remotes.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for supported targets, the native
development build and release validation boundaries.

## Arch Linux candidate

Build with `./packaging/container-build.sh arch`, then run
`./packaging/test-package.sh arch`. The native `.pkg.tar.zst` is written to
`build-out/arch/`; install a qualified candidate with `sudo pacman -U PACKAGE`.
The PKGBUILD packages the payload tested in the Arch container, including the
standalone app and desktop integration. It is not an AUR submission recipe.
Arch is a rolling target; rebuild against current repositories before release.
The missing parallel-hashmap headers come from a checksummed upstream 2.0.0
archive; their source and license are included in the package.
The complete release set requires the qualified Arch package and its extra
source archive alongside both Fedora architectures, Debian and Ubuntu.

## Flatpak assessment

A standalone Flatpak edition is feasible for creation, inspection and extraction,
but it cannot replace the native package's host Dolphin plugins, KIO worker and
Plasma integration. Flatpak installs application exports in specific locations;
it does not install shared libraries into the host's Qt plugin directories.
See [Flatpak desktop integration](https://docs.flatpak.org/en/latest/desktop-integration.html).

The installed KDE 6.11 SDK sandbox was checked: it has no `/dev/fuse` device or
`fusermount3`. Mounts inside a sandbox also do not provide the current host-visible
volume workflow. A future edition should use the native Mounts1 D-Bus service as
an optional companion, and disable mount actions when that service is absent.
Its About action currently delegates to the tray and would also need a local
dialog. File chooser grants, output paths and shared preferences need explicit
portal/sandbox testing. See [Flatpak permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html).

For the first public release, ship native packages. Treat Flatpak as a subsequent
standalone edition with an explicit feature boundary, rather than advertising a
manifest that has not been built or qualified.
