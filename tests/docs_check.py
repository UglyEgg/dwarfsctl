#!/usr/bin/env python3
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]

expected = {
    "README.md",
    "CONTRIBUTING.md",
    "CHANGELOG.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "docs/DBUS_API.md",
    "packaging/README.md",
    "kde/tray/resources/about/CREDITS.md",
}

tracked = {
    str(path.relative_to(root))
    for pattern in ["*.md", "docs/*.md", "packaging/*.md", "kde/*/*/*/*.md"]
    for path in root.glob(pattern)
    if path.is_file()
}
if tracked != expected:
    added = sorted(tracked - expected)
    missing = sorted(expected - tracked)
    raise SystemExit(
        f"unexpected public documentation inventory: added={added}, missing={missing}"
    )

link = re.compile(r"(?<!!)\[[^]]+\]\(([^)]+)\)")
for relative in sorted(expected):
    path = root / relative
    text = path.read_text()
    for target in link.findall(text):
        target = target.split("#", 1)[0]
        if not target or "://" in target or target.startswith("mailto:"):
            continue
        destination = (path.parent / target).resolve()
        if not destination.exists():
            raise SystemExit(f"broken documentation link: {relative} -> {target}")

for retired in [
    "mkdocs.yml",
    "AGENTS.md",
    "TASKS.md",
    "SPEC.md",
    "BUILD_FIX_TAG.txt",
    "docs/requirements.txt",
    "docs/internal",
    "RELEASE_SIGNING_KEY.pub",
]:
    if (root / retired).exists():
        raise SystemExit(f"retired public-development document returned: {retired}")

release_key = (root / "RELEASE_SIGNING_KEY.asc").read_text()
if "-----BEGIN PGP PUBLIC KEY BLOCK-----" not in release_key:
    raise SystemExit("release-signing key is not an armored GPG public key")

version = (root / "VERSION").read_text().strip()
manual = (root / "man/dwarfsctl.1").read_text()
if f'"dwarfsctl {version}"' not in manual:
    raise SystemExit("manual version does not match VERSION")

print("Public documentation inventory and links passed")
