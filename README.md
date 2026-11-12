<p align="center">
  <img src="kde/tray/resources/dwarfsctl-app.svg" width="128" alt="DwarFSctl application icon">
</p>

<h1 align="center">DwarFSctl</h1>

<p align="center">DwarFS images, integrated with KDE Plasma and Dolphin.</p>

<p align="center">
  <a href="https://github.com/UglyEgg/dwarfsctl/actions/workflows/ci.yml"><img src="https://github.com/UglyEgg/dwarfsctl/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
</p>

DwarFSctl adds creation, browsing, mounting, extraction, file previews and image
details to Dolphin, with a tray application for mounted volumes and defaults.

The application ships the DwarFS engine it was tested with. Users do not need
to install DwarFS command-line tools or Rust.

> DwarFSctl is an independent project. It is not affiliated with or endorsed by
> KDE or the DwarFS project.

## Install

Release packages target Fedora 45 on x86-64 and ARM64, plus Debian 13 and
Ubuntu 26.04 on x86-64.
Download the package for your distribution from the
[latest release](https://github.com/UglyEgg/dwarfsctl/releases/latest), then
install it with the distribution package manager:

```sh
# Fedora
sudo dnf install ./dwarfs-dolphin-*.rpm

# Debian or Ubuntu
sudo apt install ./dwarfs-dolphin_*.deb
```

Log out and back in after the first installation so Plasma and Dolphin discover
every component. Package upgrades normally need only a Dolphin and tray restart.

## Use it

In Dolphin:

- Right-click a folder and choose **DwarFS → Build Image…**.
- Open a `.dwarfs` file to mount it and show the volume in Places.
- Browse an image without mounting it through its `dwarfs:` location.
- Open **Properties → DwarFS Image** for contents, compression and mount state.
- Use the file menu to extract, check integrity or work with checksums.
- Open tray **Settings…** to choose creation and mount behavior.

Open **DwarFS Utility** from the application launcher when you want to inspect
or manage several images in one place. Its Overview compares the original and
compressed sizes; Contents shows a bounded visual map of the image hierarchy.

The command-line interface exposes the same engine for scripts:

```sh
dwarfsctl build ./folder --output ./archive.dwarfs
dwarfsctl status ./archive.dwarfs
dwarfsctl mount ./archive.dwarfs
dwarfsctl extract ./archive.dwarfs ./restored
dwarfsctl unmount ./archive.dwarfs
```

Run `man dwarfsctl` or `dwarfsctl COMMAND --help` for the version-matched command
reference.

### Verify a release

Release downloads include `SHA256SUMS` and its armored GPG signature,
`SHA256SUMS.asc`. Import [the repository's release key](RELEASE_SIGNING_KEY.asc)
and confirm its complete fingerprint before trusting the manifest:

```sh
gpg --import RELEASE_SIGNING_KEY.asc
gpg --fingerprint 60C38F37374C2B51BE7CA8CC698CE0575AED79AA
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum --check SHA256SUMS
```

## What is included

- `dwarfsctl`, the public CLI and configuration service
- a private engine built from pinned DwarFS 0.15.7 source
- Dolphin file actions and a Properties page
- read-only `dwarfs:` browsing through KIO
- a Plasma tray application and D-Bus mount service
- DwarFS Utility, an optional standalone workspace with exact compression and
  bounded Content Rings views for managing several images
- MIME identification, icons and thumbnails for `.dwarfs` files

Images and mounted volumes are read-only. Extract files to a normal directory
before editing them. Browsing through `dwarfs:` does not show modification times;
mount the image when those timestamps matter.

## Build and contribute

The supported build uses Podman to create and test a native package for one of
the target distributions:

```sh
./packaging/container-build.sh fedora   # or debian, ubuntu
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for the architecture, development build,
tests and contribution expectations. The stable mount-service interface is
documented in [docs/DBUS_API.md](docs/DBUS_API.md).

## Security and licensing

Report vulnerabilities through GitHub's private
[security advisory form](https://github.com/UglyEgg/dwarfsctl/security/advisories/new).

DwarFSctl is licensed under GPL-3.0-or-later. Packages include the corresponding
application and pinned DwarFS source, along with third-party notices. See
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Copyright © 2026 Richard Majewski · Varanid Works.
