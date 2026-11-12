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

After all four supported native packages and both source archives have been
qualified, assemble and sign the release set with:

```sh
./packaging/release-artifacts.sh ./release \
  build-out/fedora-x86_64 build-out/fedora-aarch64 \
  build-out/debian build-out/ubuntu
```

The script creates `SHA256SUMS` and `SHA256SUMS.asc`. It uses GPG key
`60C38F37374C2B51BE7CA8CC698CE0575AED79AA` by default; set
`DWARFSCTL_SIGNING_KEY` to select another secret key explicitly. It never uses
the SSH keys configured for Git remotes.

See [CONTRIBUTING.md](../CONTRIBUTING.md) for supported targets, the native
development build and release validation boundaries.
